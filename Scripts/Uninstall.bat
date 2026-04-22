@echo off
setlocal EnableDelayedExpansion

echo ============================================================
echo   HPL-Compiler Uninstaller
echo ============================================================
echo.

REM Resolve the compiler directory relative to this script.
set "SCRIPT_DIR=%~dp0"
set "COMPILER_DIR=%SCRIPT_DIR%..\x64\Debug"

REM Canonicalise to an absolute path.
for %%i in ("%COMPILER_DIR%") do set "COMPILER_DIR=%%~fi"

REM Check if the compiler directory exists.
if not exist "%COMPILER_DIR%" (
    echo [INFO] Compiler directory not found:
    echo        %COMPILER_DIR%
    echo Nothing to uninstall.
    echo.
    pause
    exit /b 0
)

REM Remove HPL.bat shim if it exists.
if exist "%COMPILER_DIR%\HPL.bat" (
    del "%COMPILER_DIR%\HPL.bat" >nul 2>&1
    if exist "%COMPILER_DIR%\HPL.bat" (
        echo [WARN] Could not delete HPL.bat
    ) else (
        echo [OK] Removed HPL.bat shortcut.
    )
) else (
    echo [INFO] No HPL.bat shortcut found.
)
echo.

REM Read current user PATH.
for /f "tokens=2*" %%a in (
    'reg query "HKCU\Environment" /v PATH 2^>nul'
) do set "USER_PATH=%%b"

if "!USER_PATH!"=="" (
    echo [INFO] No PATH entry found in user environment.
    echo Nothing to remove.
    echo.
    pause
    exit /b 0
)

REM Remove the compiler directory from PATH.
set "SEARCH=%COMPILER_DIR%"
set "NEW_PATH=!USER_PATH:%SEARCH%=!"

REM Clean up possible double semicolons.
set "NEW_PATH=!NEW_PATH:;;=;!"

REM Trim leading/trailing semicolons.
if "!NEW_PATH:~0,1!"==";" set "NEW_PATH=!NEW_PATH:~1!"
if "!NEW_PATH:~-1!"==";" set "NEW_PATH=!NEW_PATH:~0,-1!"

REM Update PATH.
reg add "HKCU\Environment" /v PATH /t REG_EXPAND_SZ /d "!NEW_PATH!" /f >nul

if errorlevel 1 (
    echo [ERROR] Failed to update PATH. Try running as Administrator.
    echo.
    pause
    exit /b 1
)

REM Broadcast environment change.
powershell -NoProfile -Command ^
    "[System.Environment]::SetEnvironmentVariable('PATH', [System.Environment]::GetEnvironmentVariable('PATH','User'), 'User')" ^
    >nul 2>&1

echo [OK] PATH updated successfully.
echo.
echo   Removed: %COMPILER_DIR%
echo.
echo ============================================================
echo   HPL-Compiler has been uninstalled.
echo   Close and reopen any terminals for changes to apply.
echo ============================================================
echo.
pause
endlocal
