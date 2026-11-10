param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root ('artifacts/memory-service/'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
$out=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $out.StartsWith((Join-Path $root 'artifacts')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Use an artifacts directory.'}
[void][IO.Directory]::CreateDirectory($out)
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
$msvc=(Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory|Sort-Object Name -Descending|Select-Object -First 1).FullName
$sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
$sdkVersion=(Get-ChildItem (Join-Path $sdk 'Include') -Directory|Where-Object {$_.Name -match '^10\.'}|Sort-Object Name -Descending|Select-Object -First 1).Name
$saved=@{};foreach($name in @('INCLUDE','LIB','PATH')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
  $env:INCLUDE=(Join-Path $msvc 'include')+';'+((@('ucrt','shared','um')|ForEach-Object {Join-Path $sdk "Include/$sdkVersion/$_"}) -join ';')
  $env:LIB=(Join-Path $msvc 'lib/x64')+';'+(Join-Path $sdk "Lib/$sdkVersion/ucrt/x64")+';'+(Join-Path $sdk "Lib/$sdkVersion/um/x64")
  $env:PATH=(Join-Path $sdk "bin/$sdkVersion/x64")+';'+$saved.PATH
  $exe=Join-Path $out 'MagicMemoryService.exe'
  Push-Location $out
  try{
    & (Join-Path $msvc 'bin/Hostx64/x64/cl.exe') /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /permissive- /DUNICODE /D_UNICODE (Join-Path $root 'src/memory-service/main.cpp') ("/Fe"+$exe) /link /SUBSYSTEM:CONSOLE advapi32.lib *> (Join-Path $out 'build.log')
    if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out 'build.log') -Tail 30|Out-Host;throw 'Memory service build failed.'}
  }finally{Pop-Location}
  $receipt=[ordered]@{protocol='Magic.MemoryService/1';executable=$exe;bytes=(Get-Item $exe).Length;sha256=(Get-FileHash $exe).Hash;output=$out}
  $receipt|ConvertTo-Json|Set-Content (Join-Path $out 'build.json') -Encoding UTF8
  $receipt|ConvertTo-Json
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}