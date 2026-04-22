@echo off
cd /d "%~dp0..\..\x64\Debug"

echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Runtime\OutOfBounds.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Runtime\GotTextInsteadOfNumber.hpl
