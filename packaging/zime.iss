#ifndef AppVersion
  #error AppVersion must be supplied by build-installer.ps1
#endif
#ifndef AppVersionNumeric
  #error AppVersionNumeric must be supplied by build-installer.ps1
#endif
#ifndef StageDir
  #error StageDir must be supplied by build-installer.ps1
#endif
#ifndef OutputDir
  #error OutputDir must be supplied by build-installer.ps1
#endif
#ifndef OutputBase
  #define OutputBase "casual_ime-" + AppVersion + "-x64"
#endif

#define AppName "随意五笔输入法"
#define BrokerMutex "Local\ZIme.Broker.v2"
#define BrokerShutdownEvent "Local\ZIme.Broker.Shutdown.v2"

[Setup]
AppId={{7A9BB64E-AEDB-4B35-AC2F-93F01794D65A}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ZIme Project
DefaultDirName={autopf}\casual_ime
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
SetupArchitecture=x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern dynamic
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
Uninstallable=yes
VersionInfoVersion={#AppVersionNumeric}
VersionInfoCompany=ZIme Project
VersionInfoDescription={#AppName}安装程序
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
SetupIconFile={#StageDir}\zime.ico
UninstallDisplayIcon={app}\zime.ico
SetupLogging=yes
AllowCancelDuringInstall=yes

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Files]
Source: "{#StageDir}\casual_ime32.dll"; DestDir: "{app}"; Flags: ignoreversion 32bit; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\casual_ime.dll"; DestDir: "{app}"; Flags: ignoreversion 64bit; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\dict.idx"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\zime.ico"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\default-config.ini"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\ico\*.png"; DestDir: "{app}\ico"; Flags: ignoreversion; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\zime_registrar32.exe"; DestDir: "{app}"; Flags: ignoreversion 32bit; BeforeInstall: LogInstallingFile
Source: "{#StageDir}\zime_registrar.exe"; DestDir: "{app}"; Flags: ignoreversion 64bit; BeforeInstall: LogInstallingFile
; Copy the Broker last. A loaded TIP cannot relaunch it while earlier files are staged.
Source: "{#StageDir}\zime_broker.exe"; DestDir: "{app}"; Flags: ignoreversion; BeforeInstall: LogInstallingFile

[InstallDelete]
Type: files; Name: "{app}\zime_install.log"

[UninstallRun]
Filename: "{app}\zime_registrar32.exe"; Parameters: "--unregister ""{app}\casual_ime32.dll"""; RunOnceId: "ZImeUnregister32"; Flags: runhidden waituntilterminated logoutput skipifdoesntexist 32bit
Filename: "{app}\zime_registrar.exe"; Parameters: "--unregister ""{app}\casual_ime.dll"""; RunOnceId: "ZImeUnregister64"; Flags: runhidden waituntilterminated logoutput skipifdoesntexist 64bit

[UninstallDelete]
Type: files; Name: "{app}\zime_broker.exe.uninstall-old"
Type: files; Name: "{app}\zime_install.log"
Type: dirifempty; Name: "{app}\ico"
Type: dirifempty; Name: "{app}"

[Code]
const
  EVENT_MODIFY_STATE = $0002;
  SYNCHRONIZE = $00100000;
  WAIT_OBJECT_0 = 0;
  WAIT_ABANDONED = $00000080;
  WAIT_TIMEOUT = $00000102;
  PM_REMOVE = 1;

type
  { Pascal Script records are packed. This explicit layout matches the
    48-byte Win64 MSG structure, including alignment and lPrivate. }
  TZImeMessage = record
    Window: HWND;
    MessageCode: DWORD;
    AlignmentPadding: DWORD;
    WParam: NativeUInt;
    LParam: NativeInt;
    MessageTime: DWORD;
    PointX: Integer;
    PointY: Integer;
    PrivateData: DWORD;
  end;

var
  BrokerRenamedForInstall: Boolean;
  BrokerInstallOldPath: String;
  InstallLogLabel: TNewStaticText;
  InstallLogMemo: TNewMemo;
  RegistrationLogLineCount: Integer;

function OpenEvent(dwDesiredAccess: DWORD; bInheritHandle: Boolean;
  lpName: String): THandle;
  external 'OpenEventW@kernel32.dll stdcall';
function OpenMutex(dwDesiredAccess: DWORD; bInheritHandle: Boolean;
  lpName: String): THandle;
  external 'OpenMutexW@kernel32.dll stdcall';
function SetEvent(hEvent: THandle): Boolean;
  external 'SetEvent@kernel32.dll stdcall';
function WaitForSingleObject(hHandle: THandle; dwMilliseconds: DWORD): DWORD;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(hObject: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function PeekMessage(var MessageRecord: TZImeMessage; Window: HWND;
  FilterMin, FilterMax, RemoveMessage: UINT): Boolean;
  external 'PeekMessageW@user32.dll stdcall';
function TranslateMessage(var MessageRecord: TZImeMessage): Boolean;
  external 'TranslateMessage@user32.dll stdcall';
function DispatchMessage(var MessageRecord: TZImeMessage): NativeInt;
  external 'DispatchMessageW@user32.dll stdcall';
function GetTickCount64(): Int64;
  external 'GetTickCount64@kernel32.dll stdcall';

procedure ProcessPendingMessages();
var
  MessageRecord: TZImeMessage;
begin
  if SizeOf(MessageRecord) <> 48 then
    RaiseException('Internal error: unexpected Win64 MSG structure size.');

  while PeekMessage(MessageRecord, 0, 0, 0, PM_REMOVE) do
  begin
    TranslateMessage(MessageRecord);
    DispatchMessage(MessageRecord);
  end;
end;

procedure AddInstallLog(const MessageText: String);
var
  Timestamp: String;
begin
  Timestamp := GetDateTimeString('hh:nn:ss', '-', ':');
  if InstallLogMemo <> nil then
  begin
    InstallLogMemo.Lines.Add(Timestamp + '  ' + MessageText);
    InstallLogMemo.SelStart := Length(InstallLogMemo.Text);
  end;
  Log(MessageText);
end;

procedure RefreshRegistrationLog();
var
  Lines: TArrayOfString;
  Index: Integer;
begin
  if not LoadStringsFromLockedFile(
      ExpandConstant('{app}\zime_install.log'), Lines) then
    exit;

  for Index := RegistrationLogLineCount to GetArrayLength(Lines) - 1 do
  begin
    if InstallLogMemo <> nil then
    begin
      InstallLogMemo.Lines.Add(Lines[Index]);
      InstallLogMemo.SelStart := Length(InstallLogMemo.Text);
    end;
    Log('DLL: ' + Lines[Index]);
  end;
  RegistrationLogLineCount := GetArrayLength(Lines);
end;

function ReadRegistrationResult(const ResultPath: String;
  var ExitCode: Integer): Boolean;
var
  Lines: TArrayOfString;
begin
  Result := False;
  if not LoadStringsFromLockedFile(ResultPath, Lines) or
     (GetArrayLength(Lines) = 0) or
     (Pos('exit=', Lines[0]) <> 1) then
    exit;

  ExitCode := StrToIntDef(Copy(Lines[0], 6, MaxInt), -1);
  Result := True;
end;

procedure RunRegistration(const Architecture, HelperPath, DllPath,
  ResultPath: String);
var
  LaunchResult: Integer;
  RegistrationExitCode: Integer;
  StartedAt: Int64;
begin
  DeleteFile(ResultPath);
  AddInstallLog('开始注册 ' + Architecture + ' 输入法组件。');
  WizardForm.StatusLabel.Caption := '正在注册 ' + Architecture +
    ' 输入法组件，详细过程见下方日志。';
  WizardForm.ProgressGauge.Style := npbstMarquee;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.Refresh;

  if not Exec(HelperPath,
              '--register "' + DllPath + '" "' + ResultPath + '"',
              ExpandConstant('{app}'),
              SW_HIDE,
              ewNoWait,
              LaunchResult) then
  begin
    RaiseException('无法启动 ' + Architecture + ' 注册进程：' +
      SysErrorMessage(LaunchResult));
  end;

  StartedAt := GetTickCount64();
  while not ReadRegistrationResult(ResultPath, RegistrationExitCode) do
  begin
    RefreshRegistrationLog();
    ProcessPendingMessages();
    if Terminated then
      exit;
    if GetTickCount64() - StartedAt > 30000 then
    begin
      RaiseException(Architecture + ' 输入法组件注册超过 30 秒。' + #13#10 +
        '详细日志：' + ExpandConstant('{app}\zime_install.log'));
    end;
    Sleep(50);
  end;

  RefreshRegistrationLog();
  DeleteFile(ResultPath);
  if RegistrationExitCode <> 0 then
  begin
    RaiseException(Architecture + ' 输入法组件注册失败，退出代码：' +
      IntToStr(RegistrationExitCode) + #13#10 +
      '详细日志：' + ExpandConstant('{app}\zime_install.log'));
  end;
  AddInstallLog(Architecture + ' 输入法组件注册完成。');
end;

procedure InitializeWizard();
begin
  InstallLogLabel := TNewStaticText.Create(WizardForm);
  InstallLogLabel.Parent := WizardForm.InstallingPage;
  InstallLogLabel.Left := 0;
  InstallLogLabel.Top := WizardForm.ProgressGauge.Top +
    WizardForm.ProgressGauge.Height + ScaleY(12);
  InstallLogLabel.Caption := '详细安装日志：';

  InstallLogMemo := TNewMemo.Create(WizardForm);
  InstallLogMemo.Parent := WizardForm.InstallingPage;
  InstallLogMemo.Left := 0;
  InstallLogMemo.Top := InstallLogLabel.Top + InstallLogLabel.Height + ScaleY(4);
  InstallLogMemo.Width := WizardForm.InstallingPage.ClientWidth;
  InstallLogMemo.Height := WizardForm.InstallingPage.ClientHeight -
    InstallLogMemo.Top;
  InstallLogMemo.Anchors := [akLeft, akTop, akRight, akBottom];
  InstallLogMemo.ReadOnly := True;
  InstallLogMemo.ScrollBars := ssVertical;
  InstallLogMemo.WordWrap := True;
  InstallLogMemo.TabStop := False;
  InstallLogMemo.Font.Name := 'Consolas';
  InstallLogMemo.Font.Size := 9;
end;

procedure LogInstallingFile();
begin
  AddInstallLog('安装文件：' + ExtractFileName(CurrentFilename()));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    RegistrationLogLineCount := 0;
    AddInstallLog('开始复制和配置输入法文件。');
  end
  else if CurStep = ssPostInstall then
  begin
    RunRegistration('32 位',
      ExpandConstant('{app}\zime_registrar32.exe'),
      ExpandConstant('{app}\casual_ime32.dll'),
      ExpandConstant('{tmp}\zime-register32.result'));
    if Terminated then
      exit;
    RunRegistration('64 位',
      ExpandConstant('{app}\zime_registrar.exe'),
      ExpandConstant('{app}\casual_ime.dll'),
      ExpandConstant('{tmp}\zime-register64.result'));
    WizardForm.ProgressGauge.Style := npbstNormal;
    WizardForm.ProgressGauge.Position := WizardForm.ProgressGauge.Max;
    AddInstallLog('输入法安装与注册全部完成。');
  end;
end;

function StopBroker(): Boolean;
var
  EventHandle: THandle;
  MutexHandle: THandle;
  WaitResult: DWORD;
  StartedAt: Int64;
begin
  Result := True;
  EventHandle := OpenEvent(EVENT_MODIFY_STATE, False, '{#BrokerShutdownEvent}');
  if EventHandle <> 0 then
  begin
    SetEvent(EventHandle);
    CloseHandle(EventHandle);
  end;

  MutexHandle := OpenMutex(SYNCHRONIZE, False, '{#BrokerMutex}');
  if MutexHandle = 0 then
    exit;

  StartedAt := GetTickCount64();
  repeat
    WaitResult := WaitForSingleObject(MutexHandle, 50);
    ProcessPendingMessages();
    if Terminated then
      break;
  until (WaitResult <> WAIT_TIMEOUT) or
        (GetTickCount64() - StartedAt >= 10000);
  CloseHandle(MutexHandle);
  Result := (WaitResult = WAIT_OBJECT_0) or (WaitResult = WAIT_ABANDONED);
end;

function MoveBrokerAside(const Suffix: String): Boolean;
var
  BrokerPath: String;
  OldPath: String;
begin
  Result := True;
  BrokerPath := ExpandConstant('{app}\zime_broker.exe');
  OldPath := BrokerPath + Suffix;
  if not FileExists(BrokerPath) then
  begin
    if FileExists(OldPath) and (Suffix = '.install-old') then
    begin
      BrokerRenamedForInstall := True;
      BrokerInstallOldPath := OldPath;
    end;
    exit;
  end;

  DeleteFile(OldPath);
  Result := RenameFile(BrokerPath, OldPath);
  if Result and (Suffix = '.install-old') then
  begin
    BrokerRenamedForInstall := True;
    BrokerInstallOldPath := OldPath;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not StopBroker() then
  begin
    Result := '无法停止随意五笔输入法 Broker。请关闭正在使用输入法的程序后重试。';
    exit;
  end;
  if not MoveBrokerAside('.install-old') then
    Result := '无法替换正在使用的 Broker 文件。请关闭相关程序或重新启动 Windows 后重试。';
end;

procedure DeinitializeSetup();
var
  BrokerPath: String;
begin
  if not BrokerRenamedForInstall then
    exit;

  BrokerPath := ExpandConstant('{app}\zime_broker.exe');
  if FileExists(BrokerPath) then
    DeleteFile(BrokerInstallOldPath)
  else if FileExists(BrokerInstallOldPath) then
    RenameFile(BrokerInstallOldPath, BrokerPath);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    if not StopBroker() then
      Log('Broker did not stop within 10 seconds during uninstall.');
    if not MoveBrokerAside('.uninstall-old') then
      Log('Broker executable could not be moved aside during uninstall.');
  end;
end;
