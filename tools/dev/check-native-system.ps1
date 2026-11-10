param([switch]$BuildOnly,[switch]$Integrated)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$buildArgs=@{Integrated=[bool]$Integrated}
$build=& (Join-Path $PSScriptRoot 'build-native-system.ps1') @buildArgs|ConvertFrom-Json
$out=$build.output
$env:DOTNET_CLI_HOME=Join-Path $root 'tools/dotnet-home';$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1';$env:DOTNET_CLI_TELEMETRY_OPTOUT='1';$env:DOTNET_NOLOGO='1'
$dotnet=Join-Path $root 'tools/dotnet/dotnet.exe';$project=Join-Path $root 'tools/dev/native-system-check/NativeSystemCheck.csproj'
& $dotnet publish $project -c Release --ignore-failed-sources -o (Join-Path $out 'checks') --nologo *> (Join-Path $out 'managed-check-build.log')
if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'managed-check-build.log') -Tail 25|Out-Host;throw 'Managed native-system checks failed to build.'}
$imports=Get-Content (Join-Path $out 'dependencies.txt') -Raw
if($imports -match '(?i)(webview|tauri|seelen|sluhk|msvcp|vcruntime)'){throw 'Forbidden third-party or dynamic CRT dependency in native backend.'}
$source=Get-Content (Join-Path $root 'src/system-native/*.cpp'),(Join-Path $root 'src/system-native/*.h') -Raw
if($source -match '(?im)^\s*#\s*include\s*[<"].*(native-host-patch|seelen|tauri|webview)'){throw 'Forbidden source dependency.'}
if(-not $BuildOnly){
 $report=Join-Path $out 'checks.json'
 & $dotnet (Join-Path $out 'checks/NativeSystemCheck.dll') $build.executable $build.sha256 $report *> (Join-Path $out 'checks.log')
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'checks.log') -Tail 35|Out-Host;throw ('Native system regression failed: '+$out)}
 $result=Get-Content $report -Raw|ConvertFrom-Json;if(-not $result.allPass){throw 'Native system checks did not pass.'}
}
@{allPass=$true;buildOnly=[bool]$BuildOnly;output=$out;binaryBytes=$build.bytes;dependencyCheck=$true;desktopChanged=$false;checks=$(if(-not $BuildOnly){@($result.checks.PSObject.Properties).Count}else{0})}|ConvertTo-Json
