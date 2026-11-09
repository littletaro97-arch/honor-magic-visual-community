param([string]$OutputDirectory,[string]$NativeRenderer,[string]$Dotnet='dotnet',[string]$Compiler='cl.exe')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$artifacts=Join-Path $root 'artifacts'
if(!$OutputDirectory){$OutputDirectory=Join-Path $artifacts ('icon-quality/'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8))}
$output=[IO.Path]::GetFullPath($OutputDirectory)
if(!$output.StartsWith($artifacts+'\',[StringComparison]::OrdinalIgnoreCase)-or(Test-Path -LiteralPath $output)){throw 'Use a new output directory inside this checkout artifacts directory.'}
if(!(Get-Command $Compiler -ErrorAction SilentlyContinue)){throw 'Run from an x64 Visual Studio Developer shell, or provide -Compiler.'}
[void][IO.Directory]::CreateDirectory($output)
$probe=Join-Path $PSScriptRoot 'icon-quality/IconQualityProbe.csproj'
& $Dotnet restore $probe --configfile (Join-Path $root 'src/MagicCommunity/NuGet.Config') --nologo
if($LASTEXITCODE -ne 0){throw 'Managed icon probe restore failed.'}
& $Dotnet publish $probe -c Release --no-restore -o (Join-Path $output 'managed') --nologo
if($LASTEXITCODE -ne 0){throw 'Managed icon probe build failed.'}
& $Dotnet (Join-Path $output 'managed/IconQualityProbe.dll') $output
if($LASTEXITCODE -ne 0){throw 'Managed icon probe failed.'}
$native=Join-Path $output 'TrayQualityProbe.exe'
& $Compiler /nologo /W4 /utf-8 /O2 /MT /TC ('/Fo'+(Join-Path $output 'tray-quality.obj')) ('/Fe'+$native) (Join-Path $PSScriptRoot 'icon-quality/tray-quality-probe.c') /link user32.lib gdi32.lib shell32.lib bcrypt.lib advapi32.lib ole32.lib
if($LASTEXITCODE -ne 0){throw 'Tray capture probe build failed.'}
& $native | Set-Content -LiteralPath (Join-Path $output 'native-results.json') -Encoding UTF8
if($LASTEXITCODE -ne 0){throw 'Tray capture probe failed.'}
if($NativeRenderer){
    $renderer=[IO.Path]::GetFullPath($NativeRenderer)
    $report=Join-Path $output 'icon-cache-checks.json'
    $process=Start-Process -FilePath $renderer -ArgumentList @('--check-icon-cache',('"'+$report+'"')) -WindowStyle Hidden -PassThru -Wait
    if($process.ExitCode -ne 0 -or !(Test-Path -LiteralPath $report)){throw 'Native renderer cache checks failed.'}
    $cache=Get-Content -LiteralPath $report -Raw|ConvertFrom-Json
    if(!$cache.allPass -or $cache.requestBatches -ne 8){throw 'Visible icon requests did not settle.'}
}
[pscustomobject]@{output=$output;managed=(Get-Content -LiteralPath (Join-Path $output 'managed-results.json') -Raw|ConvertFrom-Json);tray=(Get-Content -LiteralPath (Join-Path $output 'native-results.json') -Raw|ConvertFrom-Json);nativeCacheChecked=[bool]$NativeRenderer;scope='Owned fixtures only; no desktop takeover, real tray clicks or user layout changes'}|ConvertTo-Json -Depth 5
