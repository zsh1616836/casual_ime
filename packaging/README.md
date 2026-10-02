# casual_ime Installer

## Double-click build

Double-click `build-installer.bat` in the repository root, then enter a version
such as `1.1` or `1.1.2`. The Python entry point synchronizes `IME_VERSION` and
`packaging/version.txt`, configures the x64/x86 Visual Studio build directories,
and runs the existing build and packaging pipeline. The console stays open to
show the result. Outputs are `dist/casual_ime-<version>-x64.exe`, `.sha256`, and
`.build.json`; the installer includes both x64 and x86 IME DLLs.

Requirements: Windows, Python 3.8+, CMake 3.23+, Git, Visual Studio with the
Desktop development with C++ workload and Windows SDK, and Inno Setup 7.
No Python packages need to be installed. `ISCC` can specify the full path to
`ISCC.exe`. A repeated version replaces that version's output package. The
requested version stays in the source files if compilation fails.

For command-line use:

```powershell
python packaging/build_installer.py --version 1.1.2
```

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
