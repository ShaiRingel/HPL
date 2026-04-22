@echo off
set "BUILD_DIR=%~f1"
set "FILENAME=%~2"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" > nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Failed to initialize VS build environment. Check VS installation path.
    exit /b 1
)

echo [1/3] Assembling...

cd "%BUILD_DIR%"

nasm -f win64 "%FILENAME%.asm" -o "%FILENAME%.obj"

echo [2/3] Linking...

link "%FILENAME%.obj" /NODEFAULTLIB /subsystem:console /entry:Start user32.lib kernel32.lib /out:"%FILENAME%.exe"

:: Check if linking failed
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Linking failed with error code %ERRORLEVEL%. Aborting.
    :: Clean up the object file even on failure
    if exist "%FILENAME%.obj" del "%FILENAME%.obj"
    :: Exit the batch script and pass the error code back
    exit /b %ERRORLEVEL%
)

del "%FILENAME%.obj"

echo [3/3] Linking completed successfully. Running program...
timeout /t 1 /nobreak >nul
cls

"%FILENAME%.exe"