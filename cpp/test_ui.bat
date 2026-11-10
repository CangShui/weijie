@echo off
call "%~dp0..\build.bat" -Task Test -Suite UI %*
exit /b %ERRORLEVEL%
