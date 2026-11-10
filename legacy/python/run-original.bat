@echo off
chcp 65001 >nul
setlocal
set PY=C:\Users\Administrator\.workbuddy-ai\binaries\python\envs\default\Scripts\pythonw.exe
if exist "%PY%" (
  start "" "%PY%" "%~dp0src\main.py" %*
) else (
  start "" pythonw "%~dp0src\main.py" %*
)
