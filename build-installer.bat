@echo off
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
pushd "%~dp0"
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if not errorlevel 1 (
    py -3 "%~dp0packaging\build_installer.py" %*
    goto finished
)
python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if not errorlevel 1 (
    python "%~dp0packaging\build_installer.py" %*
    goto finished
)
echo Python 3.8+ was not found. Install Python and add it to PATH.
set "BUILD_RESULT=1"
goto paused
:finished
set "BUILD_RESULT=%ERRORLEVEL%"
:paused
echo.
if not "%BUILD_RESULT%"=="0" echo Build failed or cancelled. See the messages above.
pause
popd
exit /b %BUILD_RESULT%
