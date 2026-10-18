@echo off
REM Register FolderSizeExt column handler in HKCU (no admin required).
REM ASCII only.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if "%~1"=="" pause
