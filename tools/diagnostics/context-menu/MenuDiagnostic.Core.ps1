# Windows PowerShell 5.1 / PowerShell 7. Import has no side effects.
function Protect-MagicDiagnostic($Value) {
    if($null -eq $Value){return $null}
    if($Value -is [ValueType]){return $Value}
    if($Value -is [string]){
        $text=$Value
        foreach($root in @($env:USERPROFILE,$script:DataRoot,$script:PrivateDirectory,$script:ReportDirectory)){
            if($root){$text=$text.Replace([string]$root,'<local-path>')}
        }
        $text=[regex]::Replace($text,'(?i)(?:[a-z]:[\\/]|\\\\)[^"\r\n<>|]*','<path>')
        $text=[regex]::Replace($text,'S-1-5-[0-9-]+','<sid>')
        $text=[regex]::Replace($text,'(?i)[\w.+-]+@[\w.-]+\.[a-z]{2,}','<email>')
        $text=[regex]::Replace($text,'(?i)(bearer\s+|(?:token|secret|password|authorization)\s*[:=]\s*)\S+','$1<redacted>')
        if($text.Length -gt 8192){$text=$text.Substring(0,8192)+'<truncated>'}
        return $text
    }
    if($Value -is [Collections.IDictionary]){
        $out=[ordered]@{}
        foreach($key in $Value.Keys){
            if([string]$key -match '(?i)token|password|secret|authorization|cookie|clipboard|commandline|connectionstring|pipe(name)?$'){$out[[string]$key]='<omitted>'}
            else{$out[[string]$key]=Protect-MagicDiagnostic $Value[$key]}
        };return $out
    }
    if($Value -is [Collections.IEnumerable]){return ,@(foreach($entry in $Value){Protect-MagicDiagnostic $entry})}
    if($Value -is [pscustomobject]){
        $out=[ordered]@{};foreach($p in $Value.PSObject.Properties){$out[$p.Name]=$p.Value};return Protect-MagicDiagnostic $out
    }
    return $Value
}
function Write-MagicDiagnosticJson([string]$Path,$Value) {
    [IO.File]::WriteAllText($Path,(ConvertTo-Json -InputObject (Protect-MagicDiagnostic $Value) -Depth 20),[Text.UTF8Encoding]::new($false))
}
function Add-MagicTimeline([string]$Kind,$Detail,[string]$At='') {
    if(-not $At){$At=[DateTime]::UtcNow.ToString('o')}
    if($script:TimelineCount -ge 12000){$script:TimelineDropped++;return};$script:TimelineCount++
    $row=[ordered]@{at=$At;source=$Kind;detail=$Detail}
    $json=(Protect-MagicDiagnostic $row)|ConvertTo-Json -Depth 18 -Compress
    [IO.File]::AppendAllText((Join-Path $script:ReportDirectory 'timeline.jsonl'),$json+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
}
function Read-MagicTail([string]$Path,[int]$Limit=1048576) {
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){return ''}
    $file=Get-Item -LiteralPath $Path -Force
    if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Log reparse point skipped.'}
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try{
        $length=$stream.Length;$count=[int][Math]::Min($Limit,$length);[void]$stream.Seek(-$count,[IO.SeekOrigin]::End)
        $bytes=New-Object byte[] $count;$read=0
        while($read -lt $count){$n=$stream.Read($bytes,$read,$count-$read);if($n -eq 0){break};$read+=$n}
        $text=[Text.Encoding]::UTF8.GetString($bytes,0,$read)
        if($length -gt $count){$at=$text.IndexOf([char]10);if($at -ge 0){$text=$text.Substring($at+1)}else{$text=''}}
        return $text
    }finally{$stream.Dispose()}
}
function Get-MagicProcesses {
    $session=[Diagnostics.Process]::GetCurrentProcess().SessionId
    foreach($proc in Get-Process -Name MagicCommunity,MagicDesktopComposition,MagicSystemNative,MagicNativeEntry,MagicDesktopGuard,MagicTaskbarGuard,MagicWindowAction,explorer -ErrorAction SilentlyContinue){
        try{
            if($proc.SessionId -ne $session){continue}
            $path='';$version='';try{$path=$proc.Path;$version=$proc.MainModule.FileVersionInfo.ProductVersion}catch{}
            [pscustomobject]@{pid=$proc.Id;name=$proc.ProcessName;created=$proc.StartTime.ToUniversalTime().ToString('o');session=$proc.SessionId;path=$path;version=$version;cpuMs=$proc.TotalProcessorTime.TotalMilliseconds;workingSet=$proc.WorkingSet64;privateBytes=$proc.PrivateMemorySize64;handles=$proc.HandleCount;threads=$proc.Threads.Count;processSecurity=[MagicMenuDiagnostic.Observer]::Token($proc.Id)}
        }catch{}finally{$proc.Dispose()}
    }
}
function Find-MagicDataRoot {
    $record=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'MagicCommunity\Deployment\installation.json'
    if(Test-Path -LiteralPath $record){
        try{
            if((Get-Item -LiteralPath $record).Length -gt 65536){throw 'Installation record too large.'}
            $item=Get-Content -LiteralPath $record -Raw -Encoding UTF8|ConvertFrom-Json
            if($item.protocol -ceq 'Magic.Install/1' -and $item.ownerSid -ceq [Security.Principal.WindowsIdentity]::GetCurrent().User.Value -and $item.dataRoot -match '^[A-Za-z]:\\'){
                return [IO.Path]::GetFullPath([string]$item.dataRoot)
            }
        }catch{}
    }
    $fallback=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'MagicCommunity\UserData'
    if(Test-Path -LiteralPath (Join-Path $fallback '.magic-data.json')){return $fallback}
    return ''
}
function Get-MagicFileEvidence([string]$InstallDirectory) {
    foreach($name in @('MagicCommunity.exe','MagicCommunity.dll','MagicDesktopComposition.exe','MagicSystemNative.exe','MagicTrayCapture.dll')){
        $path=Join-Path $InstallDirectory $name
        if(Test-Path -LiteralPath $path -PathType Leaf){
            $item=Get-Item -LiteralPath $path
            [pscustomobject]@{name=$name;bytes=$item.Length;version=$item.VersionInfo.ProductVersion;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
        }else{[pscustomobject]@{name=$name;missing=$true}}
    }
}
function Test-MagicKnownQueryBuild([string]$InstallDirectory) {
    # Observe unknown builds, but never launch them using an assumed private CLI.
    $expected=@{
        'MagicCommunity.exe'='90D6D73FE8A61B90D88186216B0B6AE41155944240F4042CB35D6D480F0689A7'
        'MagicCommunity.dll'='249F28D275D1B0B6039722ABA480ABF47D5BE54C537DF82F20C17D7F750B8A60'
    }
    foreach($name in $expected.Keys){
        $path=Join-Path $InstallDirectory $name
        if(-not(Test-Path -LiteralPath $path -PathType Leaf)){return $false}
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $expected[$name]){return $false}
    };return $true
}
function Get-MagicShellRegistration {
    $records=@()
    foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
        $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::ClassesRoot,$view)
        try{
            foreach($scope in @('DesktopBackground','Directory\Background','Directory','Folder','*','AllFilesystemObjects','.lnk')){
                $key=$base.OpenSubKey($scope+'\shellex\ContextMenuHandlers')
                if(-not $key){continue}
                try{
                    foreach($name in @($key.GetSubKeyNames()|Select-Object -First 80)){
                        $handler=$key.OpenSubKey($name);try{$clsid=[string]$handler.GetValue('')}finally{$handler.Dispose()}
                        if($clsid -notmatch '^\{[a-fA-F0-9-]{36}\}$' -and $name -match '^\{[a-fA-F0-9-]{36}\}$'){$clsid=$name}
                        $server='';$classKey=$null
                        if($clsid -match '^\{[a-fA-F0-9-]{36}\}$'){
                            $classKey=$base.OpenSubKey('CLSID\'+$clsid+'\InprocServer32')
                            if($classKey){try{$server=[string]$classKey.GetValue('')}finally{$classKey.Dispose()}}
                        }
                        $records+=@{view=[string]$view;scope=$scope;handler=$name;clsid=$clsid;serverFile=$(if($server){[IO.Path]::GetFileName($server.Trim('"'))}else{''})}
                    }
                }finally{$key.Dispose()}
            }
        }finally{$base.Dispose()}
    };return $records
}
function Select-MagicMenuResponse($Response) {
    $result=[ordered]@{ok=$Response.ok}
    if(-not $Response.ok){$result.error=$Response.error;$result.cancelled=$Response.cancelled;return $result}
    $data=$Response.data
    foreach($key in @('cancelled','optimizedFile','compactRestricted','fullFileMenu','selectionCount','timings')){
        if($null -ne $data.PSObject.Properties[$key]){$result[$key]=$data.$key}
    }
    foreach($list in @('compact','more','items')){
        if($null -ne $data.PSObject.Properties[$list]){
            $result[$list]=@(foreach($row in $data.$list|Select-Object -First 512){
                [ordered]@{index=$row.index;label=$row.label;key=$row.key;verb=$row.verb;submenu=$row.submenu;disabled=$row.disabled;separator=$row.separator}
            })
            $result[$list+'Count']=@($data.$list).Count
        }
    }
    $result.providerBackoff=$Response.providerBackoff;return $result
}
function Export-MagicLiveLogs([DateTimeOffset]$Start,[DateTimeOffset]$End) {
    $inventory=@();$count=0
    if(-not $script:DataRoot){return [pscustomobject]@{records=0;missingRoot=$true;files=@()}}
    $paths=@(
        (Join-Path $script:DataRoot ('logs\'+$Start.LocalDateTime.ToString('yyyy-MM-dd')+'.jsonl')),
        (Join-Path $script:DataRoot ('logs\'+$End.LocalDateTime.ToString('yyyy-MM-dd')+'.jsonl')),
        (Join-Path $script:DataRoot 'runtime\desktop\composition\interaction.jsonl')
    )|Select-Object -Unique
    foreach($path in $paths){
        try{
            $exists=Test-Path -LiteralPath $path -PathType Leaf
            $inventory+=@{file=$path;exists=$exists;maxBytes=2097152}
            if(-not $exists){continue}
            foreach($line in (Read-MagicTail $path 2097152) -split '\r?\n'){
                if(-not $line.Trim() -or $line.Length -gt 65536){continue}
                try{$row=$line|ConvertFrom-Json;$at=[DateTimeOffset]::Parse([string]$row.at)}catch{continue}
                if($at -lt $Start.AddSeconds(-10) -or $at -gt $End.AddSeconds(2)){continue}
                $kind=if($row.name){[string]$row.name}else{[string]$row.phase}
                if($kind -notmatch '(?i)menu|context|shell|desktop_request_error|composition.*error|runtime-end|startup.*fail'){continue}
                Add-MagicTimeline 'application-log' @{file=[IO.Path]::GetFileName($path);entry=$row} $at.ToString('o');$count++
            }
        }catch{$inventory+=@{file=$path;error=$_.Exception.GetType().Name}}
    };return [pscustomobject]@{records=$count;files=$inventory;missingRoot=$false}
}
function Invoke-MagicReadOnlyQuery([string]$InstallDirectory,[string]$Target,[bool]$Full,[string]$Label,[string]$RuntimeDirectory='',[switch]$CaptureOwnedModules) {
    if(-not(Test-MagicKnownQueryBuild $InstallDirectory)){throw 'Installed executable does not match the verified v0.4.10 query protocol.'}
    $private=Join-Path $script:PrivateDirectory ($Label+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))
    $temp=Join-Path $private 'temp';$ownedRoot=Join-Path $private 'root'
    $worker=$null;$response=$null;$phases=@();$query=@{label=$Label;full=$Full;independentQuery=$true;notLiveClick=$true;ownedCrashObservation=$false;ownedModuleCapture=[bool]$CaptureOwnedModules;targetKind=$(if(-not $Target){'desktop-background'}elseif([IO.Directory]::Exists($Target)){'directory'}else{[IO.Path]::GetExtension($Target).ToLowerInvariant()})}
    try{
        $worker=[MagicMenuDiagnostic.QueryWorker]::new((Join-Path $InstallDirectory 'MagicCommunity.exe'),$ownedRoot,$temp,$RuntimeDirectory,[bool]$CaptureOwnedModules)
        $query.pid=$worker.Id;$clock=[Diagnostics.Stopwatch]::StartNew()
        try{$response=$worker.Open($(if($Target){$Target}else{$null}),$Full,12000)|ConvertFrom-Json;$query.response=Select-MagicMenuResponse $response}
        catch{$query.failure=$_.Exception.GetBaseException().Message;$query.exceptionType=$_.Exception.GetBaseException().GetType().FullName}
        $query.elapsedMs=$clock.ElapsedMilliseconds
        if($response -and $response.ok){
            $children=@()
            foreach($entry in @($response.data.more|Where-Object {$_.submenu -and $_.key -in @('new','openas')}|Select-Object -First 2)){
                $clock.Restart()
                try{$child=$worker.Children([int]$entry.index,12000)|ConvertFrom-Json;$children+=@{key=$entry.key;elapsedMs=$clock.ElapsedMilliseconds;response=(Select-MagicMenuResponse $child)}}
                catch{$children+=@{key=$entry.key;elapsedMs=$clock.ElapsedMilliseconds;failure=$_.Exception.GetBaseException().Message};break}
            };$query.children=$children
        }
    }catch{$query.failure=$_.Exception.GetBaseException().Message}
    finally{
        if($worker){try{$worker.Dispose();$query.workerClosed=$true}catch{$query.workerClosed=$false;$query.closeError=$_.Exception.GetBaseException().Message}finally{$query.lifecycle=$worker.Evidence}}
        foreach($trace in Get-ChildItem -LiteralPath $temp -Filter 'magic-menu-trace-*.log' -ErrorAction SilentlyContinue){
            $text=Read-MagicTail $trace.FullName 524288
            $query.traceAccessDenied=($text -match '(?i)80070005|-2147024891|UnauthorizedAccessException')
            $query.hresults=@([regex]::Matches($text,'(?i)0x[0-9a-f]{8}')|ForEach-Object {$_.Value.ToUpperInvariant()}|Select-Object -Unique)
            $safe=@(foreach($line in $text -split '\r?\n'){Protect-MagicDiagnostic $line}) -join [Environment]::NewLine
            [IO.File]::WriteAllText((Join-Path $script:ReportDirectory ($Label+'-phases.log')),$safe,[Text.UTF8Encoding]::new($false))
            foreach($line in $text -split '\r?\n'){
                if($line -match '^(\S+)\s+(.+)$'){
                    $phases+=@{at=$Matches[1];phase=$Matches[2]}
                    Add-MagicTimeline 'independent-shell-phase' @{query=$Label;phase=$Matches[2]} $Matches[1]
                }
            }
        }
        $query.lastPhase=if($phases.Count){$phases[-1]}else{$null}
        if($query.lifecycle){
            foreach($sample in $query.lifecycle.samples){Add-MagicTimeline 'independent-worker-sample' @{query=$Label;sample=$sample} $sample.at}
            foreach($module in $query.lifecycle.loadedModules.modules){Add-MagicTimeline 'independent-worker-module' @{query=$Label;module=$module} $module.firstSeen}
            Add-MagicTimeline 'independent-worker-exit' @{query=$Label;lifecycle=$query.lifecycle}
        }
        Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory ($Label+'-query.json')) $query
    };return $query
}
