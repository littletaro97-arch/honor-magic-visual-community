param([string]$OutputDirectory)
# Builds the process guard driver. Requires WDK: km headers, wdm.h, Inf2Cat,
# stampinf and inf2cat for the package. The script refuses to emit an
# unverifiable binary when the toolset is missing.
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$kits=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersion=(Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^10\.' } | Sort-Object Name -Descending | Select-Object -First 1).Name
if(-not $sdkVersion){throw 'Windows SDK is absent.'}
$kmInclude=Join-Path $kits "Include/$sdkVersion/km"
$wdm=Join-Path $kits "Include/$sdkVersion/km/wdm.h"
if(-not (Test-Path -LiteralPath $kmInclude) -or -not (Test-Path -LiteralPath $wdm)){
    throw ('WDK kernel headers are missing (' + $kmInclude + '). Install the Windows Driver Kit for this SDK version before building the driver.')
}
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root ('artifacts/process-guard/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$out=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $out.StartsWith((Join-Path $root 'artifacts')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Use an artifacts directory.'}
[void][IO.Directory]::CreateDirectory($out)
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$msvc=(Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory|Sort-Object Name -Descending|Select-Object -First 1).FullName
$saved=@{};foreach($name in @('INCLUDE','LIB','PATH')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
    $env:INCLUDE=(Join-Path $msvc 'include')+';'+((@('km','shared','ucrt','um')|ForEach-Object {Join-Path $kits "Include/$sdkVersion/$_"}) -join ';')
    $env:LIB=(Join-Path $msvc 'lib/x64')+';'+(Join-Path $kits "Lib/$sdkVersion/ucrt/x64")+';'+(Join-Path $kits "Lib/$sdkVersion/um/x64")+';'+(Join-Path $kits "Lib/$sdkVersion/km/x64")
    $env:PATH=(Join-Path $kits "bin/$sdkVersion/x64")+';'+$saved.PATH
    $sys=Join-Path $out 'MagicProcessGuard.sys'
    Push-Location $out
    try{
        & (Join-Path $msvc 'bin/Hostx64/x64/cl.exe') /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 /permissive- /DUNICODE /D_UNICODE /D_WIN64 /GS /kernel /D_KERNEL_MODE /I(Join-Path $kits "Include/$sdkVersion/km") (Join-Path $root 'src/process-guard-driver/main.cpp') /Fo:main.obj /link /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /OUT:$sys ntoskrnl.lib hal.lib wdm.lib BufferOverflowK.lib *> (Join-Path $out 'build.log')
        if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'build.log') -Tail 30|Out-Host;throw 'Process guard driver build failed.'}
    }finally{Pop-Location}
    Copy-Item -LiteralPath (Join-Path $root 'src/process-guard-driver/MagicProcessGuard.inf') -Destination $out
    $inf2cat=Join-Path $kits "bin/$sdkVersion/x64/Inf2Cat.exe"
    if(-not (Test-Path -LiteralPath $inf2cat)){throw 'Inf2Cat is missing; install the WDK to produce a signed catalogue.'}
    & $inf2cat /driver:$out /os:10_X64 /verbose *> (Join-Path $out 'inf2cat.log')
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'inf2cat.log') -Tail 20|Out-Host;throw 'Driver catalogue generation failed.'}
    $receipt=[ordered]@{protocol='Magic.ProcessGuard/1';driver=$sys;bytes=(Get-Item $sys).Length;sha256=(Get-FileHash $sys).Hash;inf=(Join-Path $out 'MagicProcessGuard.inf');catalog=(Join-Path $out 'MagicProcessGuard.cat');output=$out;signing='unsigned'}
    $receipt|ConvertTo-Json|Set-Content (Join-Path $out 'build.json') -Encoding UTF8
    $receipt|ConvertTo-Json
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}