# casual_ime Installer

`build-installer.ps1` builds the existing x64 and x86 CMake trees, validates
the frozen dictionary and runtime icons, stages the release files, and invokes
Inno Setup 7 to create one x64 installer.

The installer contains:

- x64 and x86 TSF DLLs
- the x64 per-user Broker
- the frozen base dictionary
- the eight runtime status icons from `assets/status-icons`
- an installer icon derived from `assets/status-icons/main.png`
- the immutable first-run defaults in `default-config.ini`
- x86 and x64 out-of-process registration helpers

Setup always creates an Inno Setup log and shows a live, scrolling log box on
the installing page. The x86/x64 registration helpers run asynchronously while
Setup pumps window messages, so the window remains movable and cancellable.
The DLLs also append exact ACL and registration phases to
`{app}\zime_install.log`.

The immutable defaults are copied to `%LOCALAPPDATA%\ZIme\config.ini` only when
that user file does not exist. Mutable files under `%LOCALAPPDATA%\ZIme` are
never packaged or removed by the uninstaller.

Run from the repository root:

```powershell
.\packaging\build-installer.ps1
```

Use `-SkipBuild` to package existing binaries, or `-RequireClean` for a release
that must come from a clean Git working tree. Set `ISCC` to the full path of
`ISCC.exe` only when Inno Setup 7 cannot be found automatically.

Outputs are written to `dist\` as `casual_ime-<version>-x64.exe`, together
with matching SHA-256 and build metadata files.
