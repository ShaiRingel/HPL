@echo off
cd /d "%~dp0..\..\x64\Debug"

echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Lexical\IdNotStartWithLetter.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Lexical\IdTooLong.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Lexical\InconsistentIndent.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Lexical\MaxIndentDepth.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Lexical\InvalidChar.hpl