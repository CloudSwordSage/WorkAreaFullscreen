; WorkAreaFullscreen Inno Setup 脚本
; 编译：在 installer/ 目录下执行  ISCC.exe WorkAreaFullscreen.iss
; 依赖先完成 CMake 构建：cmake --preset release; cmake --build --preset release

#define AppName        "WorkAreaFullscreen"
#ifndef AppVersion
#define AppVersion     "0.1.0"
#endif
#define AppPublisher   "WorkAreaFullscreen"
#define AppExeName     "WorkAreaFullscreen.exe"
#define InjectDllName  "injector.dll"
#define LauncherName   "HookLauncher.exe"

[Setup]
AppId={{8F2A1C3B-4D5E-4A6B-9C7D-1E2F3A4B5C6D}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
OutputDir=..\dist
OutputBaseFilename={#AppName}-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 安装到 Program Files 需要管理员权限；运行时的提权由程序按需自行请求
PrivilegesRequired=admin

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "autostart";   Description: "开机自动启动"; GroupDescription: "附加任务:"
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"; Flags: unchecked

[Files]
Source: "..\build\release\{#AppExeName}";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\{#InjectDllName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\release\{#LauncherName}"; DestDir: "{app}"; Flags: ignoreversion
; 示例配置：只在缺失时放置，避免覆盖用户已有配置
Source: "..\config\config.yaml"; DestDir: "{app}"; DestName: "config.example.yaml"; Flags: onlyifdoesntexist

[Icons]
Name: "{group}\{#AppName}";        Filename: "{app}\{#AppExeName}"
Name: "{group}\卸载 {#AppName}";   Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}";  Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
    ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExeName}"""; \
    Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExeName}"; Description: "启动 {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 配置与日志存放在 %APPDATA%，卸载时保留用户配置
Type: files; Name: "{app}\config.example.yaml"
