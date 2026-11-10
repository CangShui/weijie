@echo off
rem For OCR, use root build.bat -Task Test -Suite CPU or GPU.
call "%~dp0..\build.bat" -Task Test -Suite Core %*
exit /b %ERRORLEVEL%
