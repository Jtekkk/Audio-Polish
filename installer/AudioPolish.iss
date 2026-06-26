; Inno Setup script for the Audio Polish plugin (Windows x64).
;
; Compile with:
;   ISCC.exe /DAppVersion=1.0.0 installer\AudioPolish.iss
;
; Defines (override on the ISCC command line with /D<name>=<value>):
;   AppVersion  - version string shown in the installer        (default 1.0.0)
;   BuildDir    - path to the CMake build directory             (default ..\build)
;
; The build must have produced:
;   <BuildDir>\AudioPolish_artefacts\Release\VST3\Audio Polish.vst3\   (folder bundle)
;   <BuildDir>\AudioPolish_artefacts\Release\Standalone\Audio Polish.exe

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#ifndef BuildDir
  #define BuildDir "..\build"
#endif

#define AppName       "Audio Polish"
#define AppPublisher  "Jtekk Audio"
#define AppURL        "https://github.com/Jtekkk/Audio-Polish"
#define ArtefactDir   BuildDir + "\AudioPolish_artefacts\Release"

[Setup]
; A unique, stable id for this product. Do not reuse for other products.
AppId={{7E9C1A42-6B3D-4F58-9C21-0A5E7D3B8F64}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=AudioPolish-{#AppVersion}-Windows-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\Audio Polish.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full";    Description: "Full installation"
Name: "custom";  Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3";       Description: "VST3 plugin (for your DAW)"; Types: full custom; Flags: fixed
Name: "standalone"; Description: "Standalone application";     Types: full custom

[Files]
; The entire VST3 bundle folder goes into the shared 64-bit VST3 directory.
Source: "{#ArtefactDir}\VST3\Audio Polish.vst3\*"; \
    DestDir: "{commoncf64}\VST3\Audio Polish.vst3"; \
    Flags: ignoreversion recursesubdirs createallsubdirs; Components: vst3

; The standalone executable.
Source: "{#ArtefactDir}\Standalone\Audio Polish.exe"; \
    DestDir: "{app}"; Flags: ignoreversion; Components: standalone

[Icons]
Name: "{group}\{#AppName}";            Filename: "{app}\Audio Polish.exe"; Components: standalone
Name: "{group}\Uninstall {#AppName}";  Filename: "{uninstallexe}"

[Run]
Filename: "{app}\Audio Polish.exe"; Description: "Launch {#AppName}"; \
    Flags: nowait postinstall skipifsilent; Components: standalone

[UninstallDelete]
; Remove the VST3 bundle folder on uninstall (its contents are removed with it).
Type: filesandordirs; Name: "{commoncf64}\VST3\Audio Polish.vst3"
