@echo off
REM Unregister FolderSizeExt column handler from HKCU. ASCII only.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
if "%~1"=="" pause
