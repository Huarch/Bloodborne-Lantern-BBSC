@echo off
powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0tools\start-client.ps1" -CheckOnly
pause
