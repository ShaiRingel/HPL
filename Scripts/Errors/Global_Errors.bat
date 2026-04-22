@echo off
cd /d "%~dp0..\..\x64\Debug"

echo --------------------------------------------------------------
echo.
HPL-Compiler.exe
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe NonExistingFile.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe NonExistingFile.ppl