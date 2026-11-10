@echo off
chcp 65001 >nul
cd /d "%~dp0"
set PY=C:\Users\Administrator\.workbuddy-ai\binaries\python\envs\default\Scripts\python.exe
if not exist "%PY%" set PY=python
"%PY%" -m PyInstaller --noconfirm --clean --onefile --windowed ^
  --name 简截 --icon "%~dp0assets\icon.ico" ^
  --collect-all winrt ^
  --hidden-import winrt.windows.media.ocr ^
  --hidden-import winrt.windows.graphics.imaging ^
  --hidden-import winrt.windows.storage.streams ^
  --hidden-import winrt.windows.globalization ^
  --distpath "%~dp0dist" --workpath "%~dp0build\pyi" --specpath "%~dp0build" ^
  "%~dp0src\main.py"
echo.
echo 打包完成: dist\简截.exe
pause
