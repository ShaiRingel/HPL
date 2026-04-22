@echo off

cd /d "%~dp0"
call .\Errors\Global_Errors.bat

cd /d "%~dp0"
call .\Errors\Lexical_Errors.bat

cd /d "%~dp0"
call .\Errors\Syntax_Errors.bat

cd /d "%~dp0"
call .\Errors\Semantic_Errors.bat

pause

cd /d "%~dp0"
call .\Errors\Runtime_Errors.bat