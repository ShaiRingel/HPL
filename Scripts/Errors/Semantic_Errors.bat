@echo off
cd /d "%~dp0..\..\x64\Debug"

echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\ArithmeticTypeMismatch.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\AssignmentTypeMismatch.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\FunctionAsVariable.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\FunctionCall.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\FunctionDoesntReturn.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\IncDecNonNumber.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\NotAFunction.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\ReturnOutsideFunction.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\ReturnTypeMismatch.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\UndeclaredIdentifier.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\AlreadyDeclared.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\AtPositionOnNonText.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\ConditionNumericOperator.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\ConditionEqualityMismatch.hpl
echo --------------------------------------------------------------
echo.
HPL-Compiler.exe ..\..\ErrorExamples\Semantic\FunctionUsedAsVar.hpl