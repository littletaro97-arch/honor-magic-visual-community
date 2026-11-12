# No startup work when dot-sourced: fixture checks use isolated processes and links.
function Initialize-MagicInstallNative {
    if(-not ('MagicCommunity.Setup.InstallNative' -as [type])) { Add-Type -Path (Join-Path $PSScriptRoot 'Win32Install.Native.cs') }
}
function Get-MagicProgramFiles64 {
    if(-not [Environment]::Is64BitOperatingSystem){throw '社区版安装需要 64 位 Windows。'}
    # ProgramFiles in a WOW64 helper resolves to Program Files (x86). Read the
    # machine's protected 64-bit registry view, never a caller's environment.
    $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key=$machine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion')
        try {
            if(-not $key -or $key.GetValueKind('ProgramFilesDir') -ne [Microsoft.Win32.RegistryValueKind]::String){throw '无法核验 64 位程序目录。'}
            $value=[string]$key.GetValue('ProgramFilesDir',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if($value -notmatch '^[A-Za-z]:\\' -or $value.Contains('%')){throw '64 位程序目录记录无效。'}
            return [IO.Path]::GetFullPath($value).TrimEnd('\')
        }finally{if($key){$key.Dispose()}}
    }finally{$machine.Dispose()}
}
function Get-MagicRegisteredRoot {
    $machine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $key=$machine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{80B5D4B1-E952-4B68-B38A-368DC145B09F}_is1')
        try {
            if(-not $key){return ''}
            $value=[string]$key.GetValue('InstallLocation',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if(-not $value -or $value -notmatch '^[A-Za-z]:\\' -or $value.Contains('%') -or $key.GetValueKind('InstallLocation') -ne [Microsoft.Win32.RegistryValueKind]::String){throw '已有安装位置记录无效，请先修复安装记录。'}
            return [IO.Path]::GetFullPath($value).TrimEnd('\')
        }finally{if($key){$key.Dispose()}}
    }finally{$machine.Dispose()}
}
function Get-MagicWin32Root {
    if($script:MagicInstallTarget){return $script:MagicInstallTarget}
    $registered=Get-MagicRegisteredRoot
    if($registered){return $registered}
    Join-Path (Get-MagicProgramFiles64) 'MagicCommunityDesktop'
}
function ConvertTo-MagicInstallPath([string]$Value) {
    if(-not $Value -or $Value -notmatch '^[A-Za-z]:\\' -or $Value.Contains('"') -or $Value.Contains('%') -or $Value.Contains('/')){throw '请选择本机磁盘上的专用安装目录。'}
    $path=[IO.Path]::GetFullPath($Value).TrimEnd('\')
    if($path.Length -lt 4 -or $path.Length -gt 200){throw '安装位置不能是磁盘根目录或过长的路径。'}
    foreach($part in $Value.Substring(3).TrimEnd('\').Split('\')){
        if(-not $part -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\..*)?$'){throw '安装目录含无效名称。'}
    }
    foreach($special in @([Environment]::GetFolderPath('Windows'),[Environment]::GetFolderPath('UserProfile'),[Environment]::GetFolderPath('CommonApplicationData'),(Join-Path (Get-MagicProgramFiles64) 'WindowsApps'))){
        if($special -and ($path -ieq $special -or $path.StartsWith($special.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase))){throw '不能在系统目录、账户目录或应用数据目录内安装。'}
    }
    if($path -iin @((Get-MagicProgramFiles64),[Environment]::GetFolderPath('ProgramFilesX86'))){throw '请在程序目录中选择专用子目录。'}
    return $path
}
function Assert-MagicInstallTarget([string]$Expected) {
    $target=ConvertTo-MagicInstallPath $Expected
    $registered=Get-MagicRegisteredRoot
    if($registered -and $registered -ine $target){throw '更新会沿用原安装位置。若需另选位置，请先卸载并保留数据，再重新安装。'}
    $script:MagicInstallTarget=$target
}
function Test-MagicUnsafeWrite($Rule,[switch]$Ancestor,[switch]$LinkFile,[switch]$VolumeRoot) {
    if($Rule.AccessControlType -ne 'Allow' -or ($Rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -or $Rule.IdentityReference.Value -in @('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')){return $false}
    $mask=0x50000000L -bor [long][Security.AccessControl.FileSystemRights]::ChangePermissions -bor [long][Security.AccessControl.FileSystemRights]::TakeOwnership -bor [long][Security.AccessControl.FileSystemRights]::WriteAttributes -bor [long][Security.AccessControl.FileSystemRights]::WriteExtendedAttributes
    # A verified physical drive root cannot itself be renamed/deleted like a
    # directory. Its normal Modify ACE is not DELETE_CHILD on the protected app.
    # Still reject child deletion, ACL/owner changes and GENERIC_ALL on the root.
    if($Ancestor -and $VolumeRoot -and -not $LinkFile){$mask=0x10000000L -bor [long][Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor [long][Security.AccessControl.FileSystemRights]::ChangePermissions -bor [long][Security.AccessControl.FileSystemRights]::TakeOwnership;return ([long]$Rule.FileSystemRights -band $mask) -ne 0}
    if(-not $Ancestor){$mask=$mask -bor [long][Security.AccessControl.FileSystemRights]::Write}
    if(-not $LinkFile){$mask=$mask -bor [long][Security.AccessControl.FileSystemRights]::Delete -bor [long][Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles}
    return ([long]$Rule.FileSystemRights -band $mask) -ne 0
}
function Assert-MagicInstallAncestors([string]$Target) {
    Initialize-MagicInstallNative
    $parent=[IO.Path]::GetDirectoryName($Target)
    if(-not (Test-Path -LiteralPath $parent -PathType Container)){throw '安装目录的上级文件夹不存在，请选择已有目录中的专用子目录。'}
    for($part=$parent;$part;$part=[IO.Path]::GetDirectoryName($part)){
        $item=Get-Item -LiteralPath $part -Force
        if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or (Test-Path -LiteralPath (Join-Path $part '.git'))){throw '安装目录不能位于链接或源码工作树内。'}
        $acl=Get-Acl -LiteralPath $part
        if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin @('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')){throw '上级目录由普通账户拥有，请选择管理员保护的安装位置。'}
        $volumeRoot=[MagicCommunity.Setup.InstallNative]::IsFixedVolumeRoot($part)
        foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){if(Test-MagicUnsafeWrite $rule -Ancestor -VolumeRoot:$volumeRoot){throw ('上级目录允许普通用户替换应用：'+$part+'。请选择管理员保护的安装位置。')}}
    }
}
function Test-MagicInstalledProcessPath([string]$Path) {
    if(-not $Path){return $false}
    $names='MagicCommunity.exe','MagicNativeEntry.exe','MagicDesktopComposition.exe','MagicSystemNative.exe','MagicWindowAction.exe','MagicDesktopGuard.exe','MagicTaskbarGuard.exe','MagicNotificationPermission.exe'
    if([IO.Path]::GetFileName($Path) -notin $names){return $false}
    $directory=[IO.Path]::GetDirectoryName($Path)
    if($directory -ieq (Get-MagicWin32Root)){return $true}
    $prefix=Join-Path (Get-MagicProgramFiles64) 'WindowsApps'
    return [IO.Path]::GetDirectoryName($directory) -ieq $prefix -and [IO.Path]::GetFileName($directory) -cmatch '^MagicCommunity\.Native_\d+\.\d+\.\d+\.\d+_x64__x52pfh0dth434$'
}
function Get-MagicInstalledProcesses([switch]$CurrentSession) {
    Initialize-MagicInstallNative
    foreach($process in Get-Process -Name MagicCommunity,MagicNativeEntry,MagicDesktopComposition,MagicSystemNative,MagicWindowAction,MagicDesktopGuard,MagicTaskbarGuard,MagicNotificationPermission -ErrorAction SilentlyContinue) {
        if($CurrentSession -and $process.SessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId){continue}
        try {$identity=[MagicCommunity.Setup.InstallNative]::ReadProcess($process.Id)}
        catch {if(-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)){continue};throw ('无法核验组件 PID '+$process.Id+' 的归属，安装尚未替换文件。')}
        if(Test-MagicInstalledProcessPath $identity.Path){$identity}
    }
}
function Get-MagicInstalledPipeName([string]$Registration,[string]$Sid) {
    if(-not (Test-Path -LiteralPath $Registration -PathType Leaf)){throw '正在运行的首次配置尚未完成；请先在该配置窗口完成或取消，再在安装器内重试。'}
    $file=Get-Item -LiteralPath $Registration -Force
    if($file.Length -gt 16384 -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw '安装记录无法核验。'}
    $record=Get-Content -LiteralPath $Registration -Raw -Encoding UTF8 | ConvertFrom-Json
    if($record.protocol -cne 'Magic.Install/1' -or $record.ownerSid -cne $Sid -or $record.state -cne 'installed' -or $record.packageFamily -cnotin @('MagicCommunity.Unpackaged','MagicCommunity.Native_x52pfh0dth434')){throw '当前账户的安装记录不匹配。'}
    $root=[string]$record.dataRoot
    if(-not [IO.Path]::IsPathRooted($root) -or $root.StartsWith('\\') -or $root.Length -gt 200){throw '数据目录记录无效。'}
    $root=[IO.Path]::GetFullPath($root).TrimEnd('\')
    $sha=[Security.Cryptography.SHA256]::Create()
    try {$key=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($root.ToUpperInvariant()))).Replace('-','').Substring(0,24)}finally{$sha.Dispose()}
    return 'MagicCommunity-'+$key
}
function Request-MagicInstalledUserExit {
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $session=[Diagnostics.Process]::GetCurrentProcess().SessionId
    $own=@(Get-MagicInstalledProcesses -CurrentSession | Where-Object {$_.Sid -ceq $sid -and $_.Session -eq $session})
    if(-not $own.Count){return}
    $mains=@($own | Where-Object {[IO.Path]::GetFileName($_.Path) -ieq 'MagicCommunity.exe'})
    if(-not $mains.Count){throw '社区版正在启动或退出，主进程尚不可用。请稍候并在此安装器重试，文件尚未替换。'}
    $record=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'MagicCommunity\Deployment\installation.json'
    $pipe=Get-MagicInstalledPipeName $record $sid
    # Multiple same-image Shell workers are not the owner. Authenticate the pipe
    # server PID rather than selecting a process solely by its executable name.
    $sent=$false;$errorMessage=''
    foreach($main in $mains){
        try{[MagicCommunity.Setup.InstallNative]::RequestQuit($pipe,$main);$sent=$true;break}
        catch{$errorMessage=$_.Exception.GetBaseException().Message}
    }
    if(-not $sent){throw ('未能向当前会话发送正常退出请求：'+$errorMessage+'。可在安装器内重试。')}
    Wait-MagicInstalledExit -Sid $sid -Session $session
}
function Wait-MagicInstalledExit([string]$Sid,[int]$Session=-1,[int]$Milliseconds=30000) {
    $clock=[Diagnostics.Stopwatch]::StartNew()
    do {
        $remaining=@(Get-MagicInstalledProcesses -CurrentSession:($Session -ge 0) | Where-Object {(-not $Sid -or $_.Sid -ceq $Sid) -and ($Session -lt 0 -or $_.Session -eq $Session)})
        if(-not $remaining.Count){return}
        if($clock.ElapsedMilliseconds -ge $Milliseconds){break}
        Start-Sleep -Milliseconds 250
    }while($true)
    $details=($remaining | ForEach-Object {([IO.Path]::GetFileName($_.Path))+'（PID '+$_.Id+'，会话 '+$_.Session+'，账户 '+$_.Sid+'）'}) -join '、'
    throw ('等待社区版保存并退出超时；未强制结束进程，也未替换文件。仍在运行：'+$details+'。可在本安装器内重试。')
}
function Assert-MagicInstalledStopped {
    $remaining=@(Get-MagicInstalledProcesses)
    if($remaining.Count){
        $session=[Diagnostics.Process]::GetCurrentProcess().SessionId
        $other=@($remaining | Where-Object {$_.Session -ne $session})
        if($other.Count){throw ('其他 Windows 登录会话仍在使用社区版（会话 '+(($other.Session | Sort-Object -Unique) -join '、')+'）。安装器不会替其他账户退出桌面；请协调该会话结束使用后，在此安装器重试。')}
        Wait-MagicInstalledExit -Milliseconds 10000
    }
}
function Assert-MagicInstalledFilesFree {
    $root=Get-MagicWin32Root
    if(-not (Test-Path -LiteralPath $root -PathType Container)){return}
    # Independent check of file users, including hosts such as dotnet.exe that
    # do not share our executable names. This never calls RmShutdown.
    $files=@(Get-ChildItem -LiteralPath $root -File | Where-Object {$_.Extension -iin @('.exe','.dll') -and $_.Name -notmatch '^unins\d+\.exe$'} | ForEach-Object {$_.FullName})
    if(-not $files.Count){return}
    Initialize-MagicInstallNative
    $users=@([MagicCommunity.Setup.InstallNative]::FileUsers([string[]]$files))
    if($users.Count){
        $details=($users | ForEach-Object {'PID '+$_.Id+'，会话 '+$_.Session}) -join '；'
        throw ('安装文件仍被进程占用（'+$details+'）。未开始替换，可在安装器内重试；不会强制结束这些进程。')
    }
}

function Get-MagicShortcutRole($Link) {
    $target=[Environment]::ExpandEnvironmentVariables([string]$Link.Target)
    $arguments=([string]$Link.Arguments).Trim()
    $root=Get-MagicWin32Root
    if((Test-MagicInstalledProcessPath $target) -and [IO.Path]::GetFileName($target) -iin @('MagicNativeEntry.exe','MagicCommunity.exe')){
        if($arguments -cin @('','--desktop','--installed-desktop --desktop')){return 'main'}
        if($arguments -cin @('--configure','--installed-desktop --configure-installation')){return 'configure'}
        return ''
    }
    $id='MagicCommunity.Native_x52pfh0dth434!App'
    $parsing=[string]$Link.ParsingName;$prefix='::{4234d49b-0245-4df3-b780-3893943456e1}\'
    if(($parsing.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -and $parsing.Substring($prefix.Length) -ceq $id) -or $parsing -ceq ('shell:AppsFolder\'+$id)){return 'main'}
    if($target -ieq (Join-Path ([Environment]::GetFolderPath('Windows')) 'explorer.exe') -and $arguments.Trim('"') -ceq ('shell:AppsFolder\'+$id)){return 'main'}
    return ''
}
function Get-MagicShortcutFolders([switch]$Common) {
    if($Common){return @(@{Path=[Environment]::GetFolderPath('CommonDesktopDirectory');Desktop=$true},@{Path=[Environment]::GetFolderPath('CommonPrograms');Desktop=$false})}
    return @(@{Path=[Environment]::GetFolderPath('DesktopDirectory');Desktop=$true},@{Path=[Environment]::GetFolderPath('Programs');Desktop=$false})
}
function Get-MagicOwnedShortcuts($Folders) {
    Initialize-MagicInstallNative
    $count=0
    foreach($folder in $Folders){
        if(-not $folder.Path -or -not (Test-Path -LiteralPath $folder.Path -PathType Container)){continue}
        $queue=[Collections.Generic.Queue[string]]::new();$queue.Enqueue($folder.Path)
        while($queue.Count){
            $directory=$queue.Dequeue()
            if((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
            foreach($file in Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop){
                if(++$count -gt 8192){throw '快捷方式目录条目过多；未进行无界扫描。'}
                if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){continue}
                if($file.PSIsContainer){if(-not $folder.Desktop){$queue.Enqueue($file.FullName)};continue}
                if($file.Extension -ine '.lnk' -or $file.Length -gt 1MB){continue}
                try{$link=[MagicCommunity.Setup.InstallNative]::ReadLink($file.FullName)}catch{continue}
                $role=Get-MagicShortcutRole $link
                if($role){[pscustomobject]@{Path=$file.FullName;Role=$role;Desktop=[bool]$folder.Desktop;Hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}}
            }
        }
    }
}
function Get-MagicShortcutMask($Links) {
    $mask=0
    foreach($link in $Links){if($link.Desktop -and $link.Role -ceq 'main'){$mask=$mask -bor 1};if(-not $link.Desktop){if($link.Role -ceq 'main'){$mask=$mask -bor 2};if($link.Role -ceq 'configure'){$mask=$mask -bor 4}}}
    return $mask
}
function Assert-MagicCommonLinkProtected([string]$Path) {
    $privileged=@('S-1-5-18','S-1-5-32-544','S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
    for($part=$Path;$part;$part=[IO.Path]::GetDirectoryName($part)){
        $item=Get-Item -LiteralPath $part -Force
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '公共快捷方式路径含链接，未覆盖。'}
        $acl=Get-Acl -LiteralPath $part
        if($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $privileged){throw '公共快捷方式所有者无法核验，已保留；请在原账户修正该入口。'}
        foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
            # Delete rights on public shortcuts are normal. Content writes and
            # ACL changes are not. Updates atomically replace the link entry.
            if(Test-MagicUnsafeWrite $rule -Ancestor:$item.PSIsContainer -LinkFile){throw '公共快捷方式或上级目录允许普通用户改写内容，未以管理员权限覆盖。'}
        }
        if($part -iin @([Environment]::GetFolderPath('CommonDesktopDirectory'),[Environment]::GetFolderPath('CommonPrograms'))){break}
        if([IO.Path]::GetDirectoryName($part) -eq [IO.Path]::GetPathRoot($part)){break}
    }
}
function Get-MagicInstallTree([string]$Target) {
    $queue=[Collections.Generic.Queue[string]]::new();$queue.Enqueue($Target);$count=0
    while($queue.Count){
        $path=$queue.Dequeue();$item=Get-Item -LiteralPath $path -Force
        if(++$count -gt 8192){throw '安装文件数量异常。'}
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw '安装路径含链接，未修改。'}
        $item
        if($item.PSIsContainer){foreach($child in Get-ChildItem -LiteralPath $path -Force){$queue.Enqueue($child.FullName)}}
    }
}
function New-MagicInstallAcl {
    $acl=[Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false);$acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
    foreach($spec in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))){
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($spec[0]),$spec[1],'ContainerInherit,ObjectInherit','None','Allow'))
    }
    return $acl
}
function Sync-MagicOwnedShortcuts($Folders,[switch]$Common) {
    $root=Get-MagicWin32Root;$target=Join-Path $root 'MagicNativeEntry.exe'
    foreach($link in @(Get-MagicOwnedShortcuts $Folders)){
        $file=Get-Item -LiteralPath $link.Path -Force
        if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -or (Get-FileHash -LiteralPath $file.FullName).Hash -cne $link.Hash){throw '快捷方式在检查后发生变化，未覆盖；可重新运行安装器重试。'}
        $arguments=if($link.Role -ceq 'configure'){'--configure'}else{''}
        if($Common){Assert-MagicCommonLinkProtected $link.Path}
        $temporary=Join-Path $file.DirectoryName ('.magic-update-'+[guid]::NewGuid().ToString('N')+'.lnk')
        try {
            [MagicCommunity.Setup.InstallNative]::SaveLink($temporary,$target,$arguments,$root)
            if((Get-FileHash -LiteralPath $link.Path).Hash -cne $link.Hash){throw '快捷方式在更新前变化，已保留。'}
            # Replace the directory entry atomically, not the contents of an
            # existing file/hardlink. Never follow or execute the old link.
            [MagicCommunity.Setup.InstallNative]::ReplaceLink($temporary,$link.Path)
        } finally {if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
        $read=[MagicCommunity.Setup.InstallNative]::ReadLink($link.Path)
        if($read.Target -ine $target -or $read.Arguments -cne $arguments -or $read.Directory -ine $root -or $read.Icon -ine $target){throw '快捷方式地址未能完成核验。'}
    }
}
