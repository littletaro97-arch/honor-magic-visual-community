#ifndef Payload
  #error Payload is required
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion is required
#endif
#ifndef InstallerSuffix
  #define InstallerSuffix ""
#endif
[Setup]
AppId={{80B5D4B1-E952-4B68-B38A-368DC145B09F}
AppName=Magic 视界社区版
AppVersion={#ReleaseVersion}
AppVerName=Magic 视界社区版 {#ReleaseVersion}{#InstallerSuffix}
AppPublisher=Magic 视界社区版
AppPublisherURL=https://github.com/littletaro97-arch/honor-magic-visual-community
DefaultDirName={autopf}\MagicCommunityDesktop
DisableDirPage=auto
UsePreviousAppDir=yes
PrivilegesRequired=admin
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
DefaultGroupName=Magic 视界社区版
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\MagicNativeEntry.exe
SetupIconFile={#Payload}\Assets\MagicCommunity.ico
OutputDir={#Output}
OutputBaseFilename=MagicCommunity-Setup-{#ReleaseVersion}-x64-unsigned{#InstallerSuffix}
Compression=lzma2/fast
SolidCompression=yes
CloseApplications=no
RestartApplications=no
WizardStyle=modern
SetupLogging=yes
VersionInfoVersion={#ReleaseVersion}.0
Uninstallable=yes
SetupMutex=Global\MagicCommunity.Setup
[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："
[Files]
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "Win32Install.ps1"; Flags: dontcopy
Source: "Win32Install.ps1"; DestDir: "{app}\Setup"; Flags: ignoreversion
Source: "Win32Install.Core.ps1"; Flags: dontcopy
Source: "Win32Install.Core.ps1"; DestDir: "{app}\Setup"; Flags: ignoreversion
Source: "Win32Install.Native.cs"; Flags: dontcopy
Source: "Win32Install.Native.cs"; DestDir: "{app}\Setup"; Flags: ignoreversion
Source: "Win32Install.Cleanup.ps1"; DestDir: "{app}\Setup"; Flags: ignoreversion
Source: "Win32ProfileCleanup.cs"; DestDir: "{app}\Setup"; Flags: ignoreversion
[Icons]
Name: "{commonprograms}\Magic 视界社区版\{code:MainShortcut}"; Filename: "{app}\MagicNativeEntry.exe"; WorkingDir: "{app}"; IconFilename: "{app}\MagicNativeEntry.exe"; Check: NeedMainShortcut
Name: "{commonprograms}\Magic 视界社区版\{code:ConfigShortcut}"; Filename: "{app}\MagicNativeEntry.exe"; Parameters: "--configure"; WorkingDir: "{app}"; IconFilename: "{app}\MagicNativeEntry.exe"; Check: NeedConfigShortcut
Name: "{commondesktop}\{code:DesktopShortcut}"; Filename: "{app}\MagicNativeEntry.exe"; WorkingDir: "{app}"; IconFilename: "{app}\MagicNativeEntry.exe"; Tasks: desktopicon; Check: NeedDesktopShortcut
[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MagicCommunityDesktop"; ValueData: """{app}\MagicNativeEntry.exe"" --logon"; Flags: uninsdeletevalue
[Run]
Filename: "{app}\MagicNativeEntry.exe"; Description: "进入 Magic 视界社区版（首次运行选择数据目录）"; Flags: postinstall skipifsilent nowait runasoriginaluser
[Code]
var Maintenance: NativeUInt; ShortcutMask: Integer;
    MainLink, ConfigLink, DesktopLink: String;
function NativeCreateMutex(Attributes: NativeUInt; InitialOwner: Boolean; Name: String): NativeUInt;
  external 'CreateMutexW@kernel32.dll stdcall';
function NativeCloseHandle(Handle: NativeUInt): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
procedure ReleaseMaintenance;
begin
  if Maintenance <> 0 then begin NativeCloseHandle(Maintenance); Maintenance := 0; end;
end;
function InitializeSetup: Boolean;
begin
  Maintenance := NativeCreateMutex(0, False, 'Global\MagicCommunity.Desktop.Maintenance');
  Result := Maintenance <> 0;
end;
procedure DeinitializeSetup;
begin ReleaseMaintenance; end;
function UniqueShortcut(Base: String): String;
var Index: Integer;
begin
  Result := Base; Index := 2;
  while FileExists(Result + '.lnk') or DirExists(Result + '.lnk') do begin
    Result := Base + '（' + IntToStr(Index) + '）'; Index := Index + 1;
    if Index > 100 then RaiseException('快捷方式名称冲突过多，未覆盖已有文件。');
  end;
end;
function MainShortcut(Param: String): String; begin Result := ExtractFileName(MainLink); end;
function ConfigShortcut(Param: String): String; begin Result := ExtractFileName(ConfigLink); end;
function DesktopShortcut(Param: String): String; begin Result := ExtractFileName(DesktopLink); end;
function NeedMainShortcut: Boolean; begin Result := (ShortcutMask and 2) = 0; end;
function NeedConfigShortcut: Boolean; begin Result := (ShortcutMask and 4) = 0; end;
function NeedDesktopShortcut: Boolean; begin Result := (ShortcutMask and 1) = 0; end;
function RunGate(Path, Mode: String; Original: Boolean; var Code: Integer): Boolean;
var Params, ProgramPath: String;
begin
  Params := '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + Path + '" -Mode ' + Mode + ' -Version {#ReleaseVersion} -ExpectedInstallRoot "' + ExpandConstant('{app}') + '"';
  ProgramPath := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Log('Maintenance helper: mode=' + Mode + ', expected target=' + ExpandConstant('{app}') + ', host=' + ProgramPath + ', original user=' + IntToStr(Ord(Original)));
  if Original then Result := ExecAsOriginalUser(ProgramPath, Params, '', SW_HIDE, ewWaitUntilTerminated, Code)
  else Result := Exec(ProgramPath, Params, '', SW_HIDE, ewWaitUntilTerminated, Code);
  Log('Maintenance helper result: mode=' + Mode + ', code=' + IntToStr(Code));
  if (Mode = 'PrepareUser') or (Mode = 'ScanCommon') then begin
    Result := Result and (Code >= 64) and (Code < 72);
    if Result then Code := Code - 64;
  end else Result := Result and (Code = 0);
end;
function RetryGate(Path, Mode: String; Original: Boolean; var Code: Integer): Boolean;
begin
  repeat
    Result := RunGate(Path, Mode, Original, Code);
    if Result then Exit;
    if WizardSilent then Exit;
  until MsgBox('本次操作尚未完成。点击“重试”继续当前安装，无需关闭后重新安装。', mbError, MB_RETRYCANCEL) <> IDRETRY;
end;
function PrepareAndCheck(Path: String; var UserMask: Integer): Boolean;
var Code: Integer;
begin
  repeat
    Result := RunGate(Path, 'PrepareUser', True, UserMask);
    if Result then Result := RunGate(Path, 'Check', False, Code);
    if Result then Exit;
    if WizardSilent then Exit;
  until MsgBox('社区版尚未完成保存退出或安装检查。点击“重试”将重新请求正常退出并继续本次安装。', mbError, MB_RETRYCANCEL) <> IDRETRY;
end;
function PrepareToInstall(var NeedsRestart: Boolean): String;
var UserMask, CommonMask: Integer; Gate: String;
begin
  ExtractTemporaryFile('Win32Install.ps1');
  ExtractTemporaryFile('Win32Install.Core.ps1');
  ExtractTemporaryFile('Win32Install.Native.cs');
  Gate := ExpandConstant('{tmp}\Win32Install.ps1');
  Result := '';
  WizardForm.PreparingLabel.Caption := '正在请求社区版保存并正常退出，随后检查已有快捷方式…';
  if not PrepareAndCheck(Gate, UserMask) then
  begin Result := '正常退出或安装检查尚未完成，可重试本次安装；尚未替换文件。'; Exit; end;
  if not RetryGate(Gate, 'ScanCommon', False, CommonMask) then
  begin Result := '已有公共快捷方式尚未核验。'; Exit; end;
  ShortcutMask := UserMask or CommonMask;
  MainLink := UniqueShortcut(ExpandConstant('{commonprograms}\Magic 视界社区版\Magic 视界社区版'));
  ConfigLink := UniqueShortcut(ExpandConstant('{commonprograms}\Magic 视界社区版\启动与数据配置'));
  DesktopLink := UniqueShortcut(ExpandConstant('{commondesktop}\Magic 视界社区版'));
end;
procedure CurStepChanged(CurStep: TSetupStep);
var Code: Integer; Gate: String;
begin
  Gate := ExpandConstant('{tmp}\Win32Install.ps1');
  if CurStep = ssInstall then begin
    if not PrepareAndCheck(Gate, Code) then RaiseException('仍有组件运行或安装检查未通过，未开始覆盖。');
    if not RetryGate(Gate, 'PrepareDestination', False, Code) then RaiseException('安装目录尚未准备完成，未开始覆盖。');
  end;
  if CurStep = ssPostInstall then begin
    if not RetryGate(Gate, 'Secure', False, Code) then RaiseException('安装文件权限未完成，不能启动。');
    if not RetryGate(Gate, 'SyncCommon', False, Code) then RaiseException('公共快捷方式尚未完成更新。');
    if not RetryGate(Gate, 'SyncUser', True, Code) then RaiseException('当前账户快捷方式尚未完成更新。');
    ReleaseMaintenance;
  end;
end;
function InitializeUninstall(): Boolean;
var Code, Choice: Integer; Gate: String;
begin
  Choice := IDYES;
  if not UninstallSilent then
    Choice := MsgBox('卸载时是否保留各 Windows 账户的布局、卡片和反馈数据？' + #13#10 + #13#10 +
      '是：保留数据，重装继续沿用原位置。' + #13#10 +
      '否：删除经核验的社区版数据及数据位置记录，无法撤销。桌面原文件和收纳原文件保留。' + #13#10 +
      '取消：不卸载。', mbConfirmation, MB_YESNOCANCEL);
  Result := Choice <> IDCANCEL; if not Result then Exit;
  if Choice = IDNO then begin
    Result := MsgBox('将删除各账户的社区版布局、卡片内容、反馈及数据位置记录。确定删除并卸载？', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
    if not Result then Exit;
  end;
  Maintenance := NativeCreateMutex(0, False, 'Global\MagicCommunity.Desktop.Maintenance');
  if Maintenance = 0 then begin Result := False; Exit; end;
  Gate := ExpandConstant('{app}\Setup\Win32Install.ps1');
  Result := RunGate(Gate, 'PrepareUser', False, Code);
  if Result then Result := RunGate(Gate, 'Check', False, Code);
  if Result and (Choice = IDNO) then Result := RunGate(Gate, 'RemoveUserData', False, Code);
end;
procedure DeinitializeUninstall;
begin ReleaseMaintenance; end;
