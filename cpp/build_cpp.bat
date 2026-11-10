@echo off
rem Compatibility entry point. All outputs now live in ../Release.
call "%~dp0..\build.bat" -Task Build %*
exit /b %ERRORLEVEL%
