#requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Response','MissingWindows')][string]$Mode='Response',
    [ValidateRange(1,300)][int]$Seconds=90,
    [string]$DataRoot='',[string]$InstallDirectory='',[string]$TargetPath='',
    [string]$OutputDirectory='',[string]$RuntimeDirectory='',
    [switch]$NoPrompt,[switch]$SkipCapture,[switch]$SkipQuery,[switch]$CaptureOwnedModules,[switch]$ObserveOwnedCrash
)
$ErrorActionPreference='Stop'
# Compatibility with old command lines: debugger observation is retired, never silently re-enabled.
if($ObserveOwnedCrash){$CaptureOwnedModules=$true;Write-Warning '调试器观察已停用；本次使用普通模块枚举。'}
. (Join-Path $PSScriptRoot 'MenuDiagnostic.Core.ps1')
$runName='Magic-RightClick-'+$Mode+'-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,6)
if(-not $OutputDirectory){$OutputDirectory=Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) 'Magic右键诊断结果'}
# Unique run directory; never overwrite an existing report or application file.
$base=[IO.Path]::GetFullPath($OutputDirectory)
$sessionDirectory=Join-Path $base $runName
$script:ReportDirectory=Join-Path $sessionDirectory 'report'
$script:PrivateDirectory=Join-Path $sessionDirectory '_local-only'
[void][IO.Directory]::CreateDirectory($script:ReportDirectory)
$script:TimelineCount=0;$script:TimelineDropped=0
$started=[DateTimeOffset]::Now;$observer=$null;$queries=@();$failure=$null;$hookStatus=$null;$liveLogs=$null
$before=@();$after=@();$files=@();$policies=@();$notes=@();$clicks=0;$windowTimeouts=0
Write-Host 'Magic 视界社区版：右键诊断（不需要管理员权限）' -ForegroundColor Cyan
Write-Host ('当前入口：'+$(if($CaptureOwnedModules){'无调试器模块诊断'}elseif($SkipCapture -and $Mode -eq 'MissingWindows'){'快捷方式／子进程定点诊断'}elseif($Mode -eq 'Response'){'右键慢／无响应'}else{'Windows 功能菜单缺失'}))
Write-Host '不会安装证书、改配置、重启桌面或执行文件菜单命令。报告仅保存在本机。'
try{
    if(-not [Environment]::Is64BitProcess){throw '请使用随包 CMD 启动 64 位 Windows PowerShell。'}
    if($ExecutionContext.SessionState.LanguageMode -ne 'FullLanguage'){throw '系统策略限制了 Add-Type；请将本错误报告发回，不要关闭安全策略。'}
    Add-Type -Path @((Join-Path $PSScriptRoot 'MenuDiagnostic.Native.cs'),(Join-Path $PSScriptRoot 'MenuDiagnostic.Modules.cs'))
    $before=@(Get-MagicProcesses)
    if(-not $DataRoot){$DataRoot=Find-MagicDataRoot}
    if($DataRoot -and $DataRoot -notmatch '^[a-zA-Z]:\\'){throw '数据目录只能为本机绝对路径。'}
    $script:DataRoot=$DataRoot
    if(-not $InstallDirectory){
        $roots=@($before|Where-Object {$_.name -eq 'MagicCommunity' -and $_.path}|ForEach-Object {Split-Path $_.path}|Select-Object -Unique)
        if($roots.Count -eq 1){$InstallDirectory=$roots[0]}
        elseif($roots.Count -gt 1){$notes+='发现多处运行程序，未猜测独立查询目标。'}
        else{$InstallDirectory=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'MagicCommunityDesktop'}
    }
    if($InstallDirectory){$files=@(Get-MagicFileEvidence $InstallDirectory)}
    Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'environment.json') @{
        protocol='Magic.MenuDiagnostic/1';toolVersion='1.3';ownedCrashObservation=$false;ownedModuleCapture=[bool]$CaptureOwnedModules;mode=$Mode;started=$started.ToString('o');os=[Environment]::OSVersion.VersionString;
        culture=[Globalization.CultureInfo]::CurrentCulture.Name;uiCulture=[Globalization.CultureInfo]::CurrentUICulture.Name;
        powershell=$PSVersionTable.PSVersion.ToString();session=[Diagnostics.Process]::GetCurrentProcess().SessionId;
        elevated=([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator);
        dataRootFound=[bool]$DataRoot;installDirectory=$InstallDirectory;files=$files;processes=$before;
        versionExpected='0.4.10+760d510';observerIntervalMs=500;probeDeadlineMs=40;maximumWindowsPerProbe=4;keyboardCaptured=$false;networkUpload=$false
    }
    try{Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'shell-handlers.json') @(Get-MagicShellRegistration)}catch{$notes+='Shell 注册表部分无法读取：'+$_.Exception.GetType().Name}
    foreach($key in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer','HKLM:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer')){
        foreach($name in @('NoViewContextMenu','NoFileMenu')){
            $v=Get-ItemProperty -LiteralPath $key -Name $name -ErrorAction SilentlyContinue
            if($null -ne $v){$policies+=@{hive=$key.Split(':')[0];name=$name;value=$v.$name}}
        }
    };Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'menu-policies.json') $policies
    if(-not $SkipCapture){
        if(-not @($before|Where-Object {$_.name -eq 'MagicDesktopComposition'}).Count){$notes+='开始采集时没有找到同会话的社区版渲染器。'}
        if(-not $NoPrompt){
            Write-Host ''
            Write-Host '准备：先保持社区版运行。开始后在有问题的桌面空白处／图标上右键一次。'
            if($Mode -eq 'MissingWindows'){Write-Host '如有“显示更多选项／重试读取 Windows 菜单”，再点击一次，等待 10 秒。'}
            else{Write-Host '再打开“新建／打开方式”等出问题的子菜单，等待 10 秒。'}
            Write-Host '有条件可在 Windows 资源管理器里，对同一对象右键作对照。'
            Write-Host '采集期间不记录键盘、窗口标题、文件内容或截图。'
            [void](Read-Host '准备好后按 Enter 开始')
        }
        $captureStart=[DateTimeOffset]::Now
        $pids=[int[]]@($before|Where-Object {$_.name -ne 'explorer'}|ForEach-Object {$_.pid})
        $renderers=[int[]]@($before|Where-Object {$_.name -eq 'MagicDesktopComposition'}|ForEach-Object {$_.pid})
        $observer=[MagicMenuDiagnostic.Observer]::new($pids,$Seconds)
        Add-MagicTimeline 'capture-start' @{seconds=$Seconds;mouseHook=$observer.MouseHookInstalled;mouseHookError=$observer.MouseHookError;windowHookErrors=$observer.WindowHookErrors}
        Write-Host ('正在记录，最长 '+$Seconds+' 秒；复现后回到此窗口按 Enter 可提前结束。') -ForegroundColor Green
        $clock=[Diagnostics.Stopwatch]::StartNew();$sampleAt=0L;$processAt=0L;$until=0L
        while($clock.Elapsed.TotalSeconds -lt $Seconds -and -not $observer.Stopped){
            foreach($e in $observer.Drain()){
                Add-MagicTimeline 'input-window-event' $e $e.at
                if($e.kind -eq 'right-up'){$until=$clock.ElapsedMilliseconds+12000}
            }
            if($clock.ElapsedMilliseconds -ge $sampleAt){
                $probe=$clock.ElapsedMilliseconds -le $until
                $windows=@([MagicMenuDiagnostic.Observer]::Windows($renderers,$probe))
                foreach($w in $windows){if($w.response -eq 'timeout-or-window-ended'){$windowTimeouts++}}
                Add-MagicTimeline 'renderer-windows' @{probe=$probe;windows=$windows}
                $sampleAt=$clock.ElapsedMilliseconds+$(if($probe){500}else{2000})
            }
            if($clock.ElapsedMilliseconds -ge $processAt){
                $snapshot=@(Get-MagicProcesses)
                $renderers=[int[]]@($snapshot|Where-Object {$_.name -eq 'MagicDesktopComposition'}|ForEach-Object {$_.pid})
                $observer.UpdateOwners([int[]]@($snapshot|Where-Object {$_.name -ne 'explorer'}|ForEach-Object {$_.pid}))
                Add-MagicTimeline 'process-sample' @($snapshot|Where-Object {$_.name -ne 'explorer'}|Select-Object pid,name,created,cpuMs,workingSet,privateBytes,handles,threads,processSecurity)
                $processAt=$clock.ElapsedMilliseconds+2000
            }
            if(-not $NoPrompt -and -not [Console]::IsInputRedirected -and [Console]::KeyAvailable){
                if([Console]::ReadKey($true).Key -eq 'Enter'){break}
            }
            Start-Sleep -Milliseconds 200
        }
        $observer.Dispose()
        foreach($e in $observer.Drain()){Add-MagicTimeline 'input-window-event' $e $e.at}
        $clicks=$observer.RightClicks
        $hookStatus=@{mouseInstalled=$observer.MouseHookInstalled;mouseError=$observer.MouseHookError;windowHookErrors=$observer.WindowHookErrors;rightClicks=$clicks;dropped=$observer.Dropped;stopped=$observer.Stopped}
        $observer=$null;$captureEnd=[DateTimeOffset]::Now
        Add-MagicTimeline 'capture-end' $hookStatus
        $liveLogs=Export-MagicLiveLogs $captureStart $captureEnd
        if($clicks -eq 0){$notes+='未捕获右键松开事件：不能据此判断菜单正常；请确认采集时进行了复现，或排查权限／鼠标事件拦截。'}
    }else{$notes+='本次跳过实际点击事件采集。'}
    if($Mode -eq 'MissingWindows' -and -not $SkipQuery){
        $canQuery=$InstallDirectory -and (Test-MagicKnownQueryBuild $InstallDirectory)
        if(-not $canQuery){$notes+='程序摘要不匹配已核验的 v0.4.10，已保留现场报告并跳过独立菜单查询。'}
        else{
            if(-not $NoPrompt){
                Write-Host ''
                Write-Host '接下来只读取菜单，不执行打开、复制、删除等命令，不重启社区版。'
                Write-Host '粘贴出问题的单个文件／文件夹／快捷方式完整路径；空白直接 Enter 表示桌面空白处。'
                Write-Host '输入 S 可跳过。不要输入命令。'
                $TargetPath=Read-Host '问题对象路径'
            }
            if($TargetPath.Trim() -ieq 'S'){$notes+='用户跳过独立菜单查询。'}
            else{
                $TargetPath=$TargetPath.Trim().Trim('"')
                if($TargetPath -and ($TargetPath -notmatch '^[a-zA-Z]:\\' -or -not(Test-Path -LiteralPath $TargetPath))){$notes+='问题对象路径无效或非本机路径，未执行独立查询。'}
                else{
                    if(-not $RuntimeDirectory -and (Test-Path -LiteralPath (Join-Path $InstallDirectory 'runtime\dotnet.exe'))){$RuntimeDirectory=Join-Path $InstallDirectory 'runtime'}
                    if($CaptureOwnedModules){Write-Host '普通枚举本次查询子进程的已加载模块；不附加调试器，不生成转储。'}
                    Write-Host '读取首层与完整 Windows 菜单；每个查询最多 12 秒，可能分步等待……'
                    $queries+=Invoke-MagicReadOnlyQuery $InstallDirectory $TargetPath $false 'compact' $RuntimeDirectory -CaptureOwnedModules:$CaptureOwnedModules
                    $queries+=Invoke-MagicReadOnlyQuery $InstallDirectory $TargetPath $true 'full' $RuntimeDirectory -CaptureOwnedModules:$CaptureOwnedModules
                }
            }
        }
    }
}catch{
    $failure=$_.Exception.GetBaseException().Message
    Write-Host ('诊断未完整完成，仍将保存已有记录：'+$failure) -ForegroundColor Yellow
}finally{
    if($observer){try{$observer.Dispose()}catch{}}
    $finished=[DateTimeOffset]::Now
    try{
        $after=@(Get-MagicProcesses)
        Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'processes-after.json') $after
        $crashes=@()
        try{
            $events=Get-WinEvent -FilterHashtable @{LogName='Application';Id=1000,1001,1002,1026;StartTime=$started.AddMinutes(-1).LocalDateTime;EndTime=$finished.LocalDateTime} -MaxEvents 64 -ErrorAction Stop
            foreach($event in $events){if($event.Message -match '(?i)MagicCommunity|MagicDesktopComposition|MagicSystemNative'){$crashes+=@{at=$event.TimeCreated.ToUniversalTime().ToString('o');id=$event.Id;provider=$event.ProviderName;message=$event.Message}}}
        }catch{$notes+='Windows 事件日志没有匹配项或不可访问；不视为已排除崩溃。'}
        Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'windows-errors.json') $crashes
        $signals=@()
        $timelinePath=Join-Path $script:ReportDirectory 'timeline.jsonl'
        if(Test-Path -LiteralPath $timelinePath){
            $failureEvents=@(Get-Content -LiteralPath $timelinePath -Encoding UTF8|ForEach-Object{try{$_|ConvertFrom-Json}catch{}}|Where-Object {$_.source -eq 'application-log' -and $_.detail.entry.name -eq 'desktop_request_error'})
            $ended=@($failureEvents|Where-Object {($_.detail.entry.value|ConvertTo-Json -Compress) -match 'Shell menu worker ended'}).Count
            $expired=@($failureEvents|Where-Object {$_.detail.entry.value.message -eq 'Menu expired.'}).Count
            if($ended){$signals+=('实际请求出现 '+$ended+' 次子进程响应通道结束；其中 '+$expired+' 次同时显示菜单已失效。失效可能来自新点击取消旧查询，不能单独当作崩溃根因。')}
        }
        foreach($q in $queries){
            $serialized=$q|ConvertTo-Json -Depth 20
            if($q.traceAccessDenied -or $serialized -match '80070005|-2147024891|UnauthorizedAccessException'){ $signals+='独立查询出现访问被拒绝，与新增备用查询修复相关；仍需结合实际点击日志。' }
            if($q.lifecycle -and $q.lifecycle.exitedBeforeCleanup -and $q.lifecycle.exitCode -ne 0){$signals+=('独立 '+$q.label+' 子进程在诊断清理之前退出：'+$q.lifecycle.exitHex)}
            if($q.lifecycle -and $q.lifecycle.killRequested){$signals+=('独立 '+$q.label+' 子进程由诊断器回收，原因：'+$q.lifecycle.cleanupReason+'；这个退出码不能当成自然崩溃码。')}
            if($q.ownedModuleCapture){
                $inventory=$q.lifecycle.loadedModules
                if(-not $inventory -or -not $inventory.successfulSnapshots){$signals+=('独立 '+$q.label+' 模块枚举未取得有效快照；错误：'+$inventory.lastError+'。不将空清单视为未加载扩展。')}
                if($inventory.truncated){$signals+=('独立 '+$q.label+' 模块清单达到上限，已截断。')}
                if(@($inventory.modules|Where-Object {$_.file -ieq 'huawei_secure_c.dll'}).Count){$signals+=('独立 '+$q.label+' 观察到 huawei_secure_c.dll；这只证明模块出现，不能单独认定为原始故障原因。')}
            }
            if($q.failure){$signals+='独立 '+$q.label+' 查询未完成：'+$q.failure}
            elseif($q.response.ok -eq $false){$signals+='独立 '+$q.label+' 查询返回失败：'+$q.response.error}
            elseif($q.response.moreCount -eq 0){$signals+='独立 '+$q.label+' 查询未返回 Windows 菜单条目。'}
        }
        $timeline=Join-Path $script:ReportDirectory 'timeline.jsonl'
        if(Test-Path -LiteralPath $timeline){
            $sorted=@(Get-Content -LiteralPath $timeline -Encoding UTF8|ForEach-Object {$_|ConvertFrom-Json}|Sort-Object {try{[DateTimeOffset]::Parse($_.at).UtcDateTime.Ticks}catch{0}})
            [IO.File]::WriteAllLines((Join-Path $script:ReportDirectory 'timeline-ordered.jsonl'),[string[]]@($sorted|ForEach-Object {$_|ConvertTo-Json -Depth 18 -Compress}),[Text.UTF8Encoding]::new($false))
        }
        $status=[ordered]@{mode=$Mode;started=$started.ToString('o');finished=$finished.ToString('o');completed=($null -eq $failure);failure=$failure;hook=$hookStatus;rendererTimeoutSamples=$windowTimeouts;liveLogs=$liveLogs;independentQueries=$queries;notes=$notes;signals=$signals;timelineDropped=$script:TimelineDropped;manualAcceptance=$false}
        Write-MagicDiagnosticJson (Join-Path $script:ReportDirectory 'summary.json') $status
        $lines=@(
            'Magic 视界社区版右键诊断结果',
            ('模式：'+$Mode),
            ('记录时间：'+$started.ToString('o')+' 至 '+$finished.ToString('o')),
            ('右键松开次数：'+$clicks+'；窗口无响应或消失样本：'+$windowTimeouts),
            ('运行程序版本：'+(($before|Where-Object {$_.name -eq 'MagicCommunity'}|Select-Object -ExpandProperty version -Unique)-join '、')),
            'summary.json 为汇总；timeline-ordered.jsonl 按 UTC 排列点击、窗口、应用日志及独立查询事件，各来源单独标注。',
            'compact/full-query.json 和 phases.log 是独立只读查询，不是实际点击的内部调用栈。',
            '独立查询成功不能证明实屏显示成功；无响应样本也可能是窗口同时关闭，需要结合时间线。',
            '未读取文件内容、聊天内容、剪贴板、完整布局、令牌或内存转储；未联网发送。',
            '报告含进程 ID、窗口类、点击坐标、菜单标签、组件摘要和相关异常；路径已尽量脱敏，发送前仍可查看。',
            '请另外说明：点的是桌面空白／普通文件／文件夹／快捷方式／回收站；缺哪些功能；大约等待几秒。',
            '可附一张用户自行截取的问题菜单，以及 Windows 资源管理器对同一对象的菜单截图。',
            ('提示：'+($notes -join '；')),
            ('诊断线索：'+($signals -join '；')),
            ('采集异常：'+$failure)
        )
        [IO.File]::WriteAllLines((Join-Path $script:ReportDirectory '结果说明.txt'),[string[]]$lines,[Text.UTF8Encoding]::new($true))
        $hashes=@(Get-ChildItem -LiteralPath $script:ReportDirectory -File|ForEach-Object {((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash+'  '+$_.Name)})
        [IO.File]::WriteAllLines((Join-Path $script:ReportDirectory 'SHA256SUMS.txt'),[string[]]$hashes,[Text.UTF8Encoding]::new($false))
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip=Join-Path $base ($runName+'.zip')
        [IO.Compression.ZipFile]::CreateFromDirectory($script:ReportDirectory,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)
        Write-Host ''
        Write-Host ('请将这个 ZIP 发回：'+$zip) -ForegroundColor Cyan
        Write-Host '只发送 ZIP 即可；_local-only 为本机查询临时资料，未放入 ZIP。'
    }catch{Write-Host ('报告封包失败；已有文本保存在：'+$script:ReportDirectory+'；'+$_.Exception.Message) -ForegroundColor Red}
}
if($failure){exit 1}
