@echo off
setlocal EnableDelayedExpansion

echo ============================================================
echo   HPL-Compiler Installer
echo ============================================================
echo.

REM Resolve the compiler directory relative to this script.
REM This script lives in Scripts\, the exe lives in x64\Debug\.
set "SCRIPT_DIR=%~dp0"
set "COMPILER_DIR=%SCRIPT_DIR%..\x64\Debug"

REM Canonicalise to an absolute path (removes ..\ segments).
for %%i in ("%COMPILER_DIR%") do set "COMPILER_DIR=%%~fi"

REM Verify the executable is actually there.
if not exist "%COMPILER_DIR%\HPL-Compiler.exe" (
    echo [ERROR] HPL-Compiler.exe not found at:
    echo         %COMPILER_DIR%
    echo.
    echo Make sure you have built the project in Visual Studio
    echo ^(Build ^> Build Solution^) before running this installer.
    echo.
    pause
    exit /b 1
)

echo Compiler found at:
echo   %COMPILER_DIR%
echo.

REM Create HPL.bat shim so the user can type just "HPL" instead of "HPL-Compiler.exe".
(
    echo @echo off
    echo HPL-Compiler.exe %%*
) > "%COMPILER_DIR%\HPL.bat"

if exist "%COMPILER_DIR%\HPL.bat" (
    echo [OK] Shortcut 'HPL.bat' created.
) else (
    echo [WARN] Could not create HPL.bat shortcut.
)
echo.

REM Read the current user PATH from the registry.
for /f "tokens=2*" %%a in (
    'reg query "HKCU\Environment" /v PATH 2^>nul'
) do set "USER_PATH=%%b"

REM Check if the directory is already in PATH.
echo !USER_PATH! | findstr /i /c:"%COMPILER_DIR%" >nul 2>&1
if not errorlevel 1 (
    echo [INFO] The compiler directory is already in your PATH.
    echo        No changes were made.
    echo.
    pause
    exit /b 0
)

REM Append to user PATH (never touches the system PATH).
if "!USER_PATH!"=="" (
    set "NEW_PATH=%COMPILER_DIR%"
) else (
    set "NEW_PATH=!USER_PATH!;%COMPILER_DIR%"
)

reg add "HKCU\Environment" /v PATH /t REG_EXPAND_SZ /d "!NEW_PATH!" /f >nul

if errorlevel 1 (
    echo [ERROR] Failed to update the PATH. Try running as Administrator.
    echo.
    pause
    exit /b 1
)

REM Broadcast the environment change so open Explorer windows pick it up.
powershell -NoProfile -Command ^
    "[System.Environment]::SetEnvironmentVariable('PATH', [System.Environment]::GetEnvironmentVariable('PATH','User'), 'User')" ^
    >nul 2>&1

echo [OK] PATH updated successfully.
echo.
echo   Added: %COMPILER_DIR%
echo.
echo ============================================================
echo   You can now use HPL-Compiler from any terminal:
echo.
echo     HPL MyProgram.hpl
echo.
echo   Open a NEW terminal window for the change to take effect.
echo ============================================================
echo.
pause
endlocal