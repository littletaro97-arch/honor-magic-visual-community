param([switch]$BuildOnly,[switch]$NoBuild,[switch]$AllDisplays,[switch]$SyntheticIcons,[int]$BenchmarkCycles=0,[string]$TrialRoot)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..')).TrimEnd('\','/')
if((& git -C $root branch --show-current).Trim() -ne 'codex/features' -or -not(Test-Path -LiteralPath (Join-Path $root '.git') -PathType Leaf)){throw 'Composition prototype requires the isolated features worktree.'}
if($BenchmarkCycles -ne 0 -and ($BenchmarkCycles -lt 2 -or $BenchmarkCycles -gt 100 -or $AllDisplays)){throw 'Benchmark requires one window and 2-100 cycles.'}
$stamp=Join-Path $root 'artifacts/composition-trial-build.current.json'
function Fingerprint {
    $paths=@(& git -C $root ls-files --cached --others --exclude-standard -- 'src/MagicCommunity' 'src/composition-trial' 'src/shared' 'tools/review/*.h' 'assets/honor-icons' 'assets/honor-fonts' 'widgets/desktop' 'tools/copy-honor-fonts.ps1' 'tools/dev/check-worktree.ps1' 'tools/dev/start-composition-trial.ps1')|Sort-Object -Unique
    $rows=@($paths|ForEach-Object {$_+':'+(Get-FileHash -LiteralPath (Join-Path $root $_) -Algorithm SHA256).Hash})
    $sha=[Security.Cryptography.SHA256]::Create();try{([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($rows -join "`n"))))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
}
$fingerprint=Fingerprint
if($NoBuild){
    $receipt=Get-Content -LiteralPath $stamp -Raw -Encoding UTF8|ConvertFrom-Json
    if($receipt.sourceFingerprint -ne $fingerprint -or $receipt.root -ne $root){throw 'Prototype sources changed; rebuild first.'}
    $output=[IO.Path]::GetFullPath([string]$receipt.output)
    if(-not $output.StartsWith($root+'\artifacts\composition-trial-build\',[StringComparison]::OrdinalIgnoreCase)){throw 'Prototype output escaped worktree.'}
    foreach($file in $receipt.files){if((Get-FileHash -LiteralPath (Join-Path $output $file.name) -Algorithm SHA256).Hash -ne $file.sha256){throw ('Build hash changed: '+$file.name)}}
}else{
    $run=(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
    $output=Join-Path $root ('artifacts/composition-trial-build/'+$run);[void][IO.Directory]::CreateDirectory($output)
    & (Join-Path $root 'tools/copy-honor-fonts.ps1') -OutputDirectory $output
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
    $msvc=(Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory|Sort-Object Name -Descending|Select-Object -First 1).FullName
    $sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10';$sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $sdk 'Include') -Directory|Where-Object {$_.Name -match '^10\.'}|Sort-Object Name -Descending|Select-Object -First 1).Name
    if(-not $sdkVersion -or -not(Test-Path -LiteralPath (Join-Path $sdk "Include/$sdkVersion/cppwinrt/winrt/Windows.UI.Composition.h"))){throw 'Full Windows SDK / C++/WinRT headers are missing.'}
    $savedInclude=$env:INCLUDE;$savedLib=$env:LIB;$savedPath=$env:PATH
    try{
        $env:PATH=(Join-Path $sdk "bin/$sdkVersion/x64")+';'+(Join-Path $msvc 'bin/Hostx64/x64')+';'+$savedPath
        $env:INCLUDE=(Join-Path $msvc 'include')+';'+((@('ucrt','shared','um','winrt','cppwinrt')|ForEach-Object {Join-Path $sdk "Include/$sdkVersion/$_"}) -join ';')
        $env:LIB=(Join-Path $msvc 'lib/x64')+';'+(Join-Path $sdk "Lib/$sdkVersion/ucrt/x64")+';'+(Join-Path $sdk "Lib/$sdkVersion/um/x64")
        $native=Join-Path $output 'MagicCompositionTrial.exe'
        $arguments=@('/nologo','/std:c++20','/EHsc','/bigobj','/O2','/MD','/W4','/utf-8','/permissive-','/DUNICODE','/D_UNICODE',('/Fo'+(Join-Path $output 'composition-trial.obj')),('/Fe'+$native),(Join-Path $root 'src/composition-trial/composition-trial.cpp'),'/link','/SUBSYSTEM:WINDOWS','/MANIFEST:EMBED',('/MANIFESTINPUT:'+(Join-Path $root 'src/MagicCommunity/app.manifest')),'windowsapp.lib','d3d11.lib','d2d1.lib','dwrite.lib','dxgi.lib','user32.lib','gdi32.lib','ole32.lib','oleaut32.lib','shell32.lib','shlwapi.lib','comctl32.lib','imm32.lib','windowscodecs.lib','crypt32.lib','uiautomationcore.lib','wtsapi32.lib','dwmapi.lib')
        $text=& (Join-Path $msvc 'bin/Hostx64/x64/cl.exe') @arguments 2>&1;$code=$LASTEXITCODE;$text|Out-File -LiteralPath (Join-Path $output 'native-build.log') -Encoding UTF8
        if($code -ne 0){$text|Select-Object -Last 65|Out-Host;throw ('Native build failed; '+$output)}
    }finally{$env:INCLUDE=$savedInclude;$env:LIB=$savedLib;$env:PATH=$savedPath}
    $managed=(& (Join-Path $root 'tools/dev/check-worktree.ps1') -BuildOnly|Out-String)|ConvertFrom-Json
    $backend=Join-Path $output 'backend';Copy-Item -LiteralPath (Join-Path $managed.output 'app') -Destination $backend -Recurse
    if((Fingerprint) -ne $fingerprint){throw 'Source changed while building.'}
    $fontOutput=Join-Path $output 'fonts'
    $receipt=@{at=(Get-Date -Format o);root=$root;output=$output;nativeSha256=(Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash;sourceFingerprint=$fingerprint;sourceCommit=(& git -C $root rev-parse HEAD).Trim();msvc=$msvc;sdkVersion=$sdkVersion;scope='Isolated Composition/managed build; no tests or desktop takeover';files=@(Get-ChildItem -LiteralPath $output -Recurse -File|Where-Object {$_.Name -eq 'MagicCompositionTrial.exe' -or $_.FullName.StartsWith($backend+'\',[StringComparison]::OrdinalIgnoreCase) -or $_.FullName.StartsWith($fontOutput+'\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object {@{name=$_.FullName.Substring($output.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}})}
    $receipt|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $output 'build.json') -Encoding UTF8
    $receipt|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $stamp -Encoding UTF8
}
if($BuildOnly){$receipt|ConvertTo-Json -Depth 6;return}
$env:DOTNET_ROOT=Join-Path $root 'tools/dotnet'
$env:DOTNET_ROOT_X64=$env:DOTNET_ROOT
$exe=Join-Path $output 'backend/MagicCommunity.exe';$arguments=@('--root',('"'+$root+'"'),'--composition-desktop-trial','--composition-host',('"'+(Join-Path $output 'MagicCompositionTrial.exe')+'"'))
if($TrialRoot){$arguments+=@('--trial-root',('"'+[IO.Path]::GetFullPath($TrialRoot)+'"'))}
if($AllDisplays){$arguments+='--all-displays'}
if($SyntheticIcons){$arguments+='--synthetic-icons'}
if($BenchmarkCycles){$arguments+=@('--benchmark-cycles',[string]$BenchmarkCycles)}
$process=Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Normal
@{pid=$process.Id;executable=$exe;native=(Join-Path $output 'MagicCompositionTrial.exe');output=$output;sourceFingerprint=$fingerprint;scope='Ordinary isolated fixture windows only; close all windows to exit'}|ConvertTo-Json
