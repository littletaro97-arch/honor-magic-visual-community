param([string]$OutputDirectory,[switch]$Checks,[switch]$ProbeUnpackagedNotifications,[string]$PackagePublisher='CN=MagicCommunity.Native.Local')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root ('artifacts/native-shell-support/'+(Get-Date -Format yyyyMMdd-HHmmss))}
$out=[IO.Path]::GetFullPath($OutputDirectory)
if(-not $out.StartsWith($root+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output must remain in this source tree.'}
[void][IO.Directory]::CreateDirectory($out)
. (Join-Path $PSScriptRoot '../system-runtime/native-package-family.ps1')
$windowFamily=Get-MagicNativePackageFamily $PackagePublisher
$familyHeader=Join-Path $out 'window-package-family.h'
[IO.File]::WriteAllText($familyHeader,('#define MAGIC_WINDOW_PARENT_FAMILY L"'+$windowFamily+'"'),[Text.Encoding]::ASCII)
& (Join-Path $root 'tools/copy-honor-fonts.ps1') -OutputDirectory $out
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe';$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim();$msvc=(Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory|Sort-Object Name -Descending|Select-Object -First 1).FullName
$sdk=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10';$version=(Get-ChildItem (Join-Path $sdk Include) -Directory|Where-Object {$_.Name -match '^10\.'}|Sort-Object Name -Descending|Select-Object -First 1).Name
$saved=@{};foreach($key in @('INCLUDE','LIB','PATH')){$saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process')}
try{
 $env:INCLUDE=(Join-Path $msvc include)+';'+((@('ucrt','shared','um','winrt','cppwinrt')|ForEach-Object {Join-Path $sdk "Include/$version/$_"}) -join ';');$env:LIB=(Join-Path $msvc lib/x64)+';'+(Join-Path $sdk "Lib/$version/ucrt/x64")+';'+(Join-Path $sdk "Lib/$version/um/x64");$env:PATH=(Join-Path $sdk "bin/$version/x64")+';'+$saved.PATH
 Push-Location $out
 try{
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /O2 /MT /W4 /LD /DUNICODE /D_UNICODE (Join-Path $root src/tray-capture/tray-capture.c) /Fo:tray-capture.obj /Fe:MagicTrayCapture.dll /link /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib bcrypt.lib *> (Join-Path $out tray-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out tray-build.log) -Tail 24;throw 'Tray capture build failed.'}
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /DUNICODE /D_UNICODE (Join-Path $root src/taskbar-guard/taskbar-guard.cpp) /Fo:taskbar-guard.obj /Fe:MagicTaskbarGuard.exe /link /SUBSYSTEM:CONSOLE /DYNAMICBASE /NXCOMPAT user32.lib shell32.lib *> (Join-Path $out guard-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out guard-build.log) -Tail 24;throw 'Taskbar guard build failed.'}
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE (Join-Path $root src/notification-permission/notification-permission.cpp) /Fo:notification-permission.obj /Fe:MagicNotificationPermission.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib windowsapp.lib CoreMessaging.lib dwmapi.lib d2d1.lib dwrite.lib advapi32.lib comctl32.lib *> (Join-Path $out permission-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out permission-build.log) -Tail 24;throw 'Notification permission build failed.'}
 Copy-Item -LiteralPath (Join-Path $root 'assets/community/application.ico') -Destination (Join-Path $out 'application.ico')
 [IO.File]::WriteAllText((Join-Path $out 'application.rc'),'1 ICON "application.ico"',[Text.UTF8Encoding]::new($false))
 & (Join-Path $sdk "bin/$version/x64/rc.exe") /nologo /fo application.res application.rc *> (Join-Path $out entry-icon-build.log)
 if($LASTEXITCODE -ne 0){throw 'Native entry icon compilation failed.'}
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE (Join-Path $root src/native-package-entry/native-package-entry.cpp) application.res /Fo:native-package-entry.obj /Fe:MagicNativeEntry.exe /link /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT windowsapp.lib shell32.lib advapi32.lib user32.lib *> (Join-Path $out entry-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out entry-build.log) -Tail 24;throw 'Native package entry build failed.'}
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ("/FI"+$familyHeader) (Join-Path $root src/window-action-helper/window-action-helper.cpp) /Fo:window-action-helper.obj /Fe:MagicWindowAction.exe /link /MANIFESTUAC:NO /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /MANIFEST:EMBED ("/MANIFESTINPUT:"+(Join-Path $root src/window-action-helper/app.manifest)) windowsapp.lib user32.lib advapi32.lib shell32.lib ole32.lib uuid.lib *> (Join-Path $out window-helper-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out window-helper-build.log) -Tail 30;throw 'Window action helper build failed.'}
 if($Checks){
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE (Join-Path $root tools/review/notification-permission-appearance-check.cpp) /Fo:permission-check.obj /Fe:permission-check.exe /link /SUBSYSTEM:CONSOLE windowsapp.lib user32.lib gdi32.lib dwmapi.lib d2d1.lib dwrite.lib advapi32.lib comctl32.lib ole32.lib windowscodecs.lib *> (Join-Path $out permission-check-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out permission-check-build.log) -Tail 15;throw 'Permission appearance check build failed.'}
 & (Join-Path $out permission-check.exe) $out
 if($LASTEXITCODE -ne 0){throw 'Permission appearance checks failed.'}
 & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE (Join-Path $root tools/review/window-helper-checks.cpp) /Fo:window-helper-checks.obj /Fe:window-helper-checks.exe /link /SUBSYSTEM:CONSOLE windowsapp.lib user32.lib advapi32.lib shell32.lib ole32.lib uuid.lib *> (Join-Path $out checks-build.log)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out checks-build.log) -Tail 20;throw 'Window helper checks build failed.'}
 & (Join-Path $out window-helper-checks.exe) *> (Join-Path $out checks.json)
 if($LASTEXITCODE -ne 0){Get-Content (Join-Path $out checks.json);throw 'Window helper checks failed.'}
 }

 if($ProbeUnpackagedNotifications){
  $probeDir=Join-Path $out 'notification-identity-probe'
  [void][IO.Directory]::CreateDirectory($probeDir)
  $probeExe=Join-Path $probeDir 'probe.exe'
  & (Join-Path $msvc bin/Hostx64/x64/cl.exe) /nologo /std:c++20 /EHsc /MT /W4 /utf-8 /DUNICODE /D_UNICODE (Join-Path $root 'tools/review/notification-identity-probe.cpp') ("/Fo"+(Join-Path $probeDir 'probe.obj')) ("/Fe"+$probeExe) /link windowsapp.lib *> (Join-Path $probeDir 'compile.log')
  if($LASTEXITCODE -ne 0){throw 'Unpackaged notification probe compilation failed.'}
  $probe=Start-Process -FilePath $probeExe -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $probeDir 'result.json')
  try{
   if(-not $probe.WaitForExit(15000)){$probe.Kill();throw 'Unpackaged notification probe timed out.'}
   if($probe.ExitCode -ne 0){throw 'Unpackaged notification probe failed.'}
  }finally{$probe.Dispose()}
 }
 }finally{Pop-Location}
 $receipt=@{output=$out;packagePublisher=$PackagePublisher;packageFamily=$windowFamily;tray=Join-Path $out MagicTrayCapture.dll;guard=Join-Path $out MagicTaskbarGuard.exe;files=@(Get-ChildItem $out -File|Where-Object Extension -in @('.dll','.exe')|ForEach-Object {@{name=$_.Name;sha256=(Get-FileHash $_.FullName).Hash;bytes=$_.Length}});desktopChanged=$false};$receipt|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out build.json);$receipt|ConvertTo-Json -Depth 5
}finally{foreach($key in $saved.Keys){[Environment]::SetEnvironmentVariable($key,$saved[$key],'Process')}}
