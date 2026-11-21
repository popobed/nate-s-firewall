@echo off
setlocal EnableExtensions

rem build_and_run.bat -- entry point for Mulet Obed.
rem   build_and_run.bat            -> build, then run karo_daemon.exe interactively
rem                                    (listens for Ctrl+Alt+K; Ctrl+C to stop)
rem   build_and_run.bat selftest   -> build, then run karo_daemon.exe --selftest
rem                                    (one simulated trigger cycle, then exit;
rem                                    this is the mode used for unattended validation)

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "PROJECT_DIR_UNIX=%PROJECT_DIR:\=/%"
set "MSYS_BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%MSYS_BASH%" (
    echo [KARO] MSYS2 not found at "%MSYS_BASH%".
    echo [KARO] Install it first: winget install -e --id MSYS2.MSYS2
    exit /b 1
)

echo [KARO] Building EMPIRE_D_AUTOMATISATION ^(make clean ^&^& make ALL^)...
"%MSYS_BASH%" -lc "export PATH=/mingw64/bin:$PATH; export OS=Windows_NT; cd '%PROJECT_DIR_UNIX%'; make clean && make ALL"
if errorlevel 1 (
    echo [KARO] Build failed -- aborting.
    exit /b 1
)
echo [KARO] Build OK.
echo.

if /I "%~1"=="selftest" (
    echo [KARO] Launching karo_daemon.exe in --selftest mode ^(one simulated trigger cycle, then exit^)...
    set "KARO_DAEMON_SELFTEST=1"
) else (
    echo [KARO] Launching karo_daemon.exe. Press Ctrl+Alt+K to trigger KARO, Ctrl+C to stop.
)
echo.

"%PROJECT_DIR%\build\karo_daemon.exe"
set "DAEMON_EXIT=%ERRORLEVEL%"

echo.
echo [KARO] karo_daemon.exe exited with code %DAEMON_EXIT%.
exit /b %DAEMON_EXIT%
