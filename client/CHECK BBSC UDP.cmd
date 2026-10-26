@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\check-bbsc-udp.ps1"
set "BBSC_CHECK_EXIT=%ERRORLEVEL%"
echo.
if not "%BBSC_CHECK_EXIT%"=="0" echo BBSC server UDP check failed. Save this window for the server owner.
pause
exit /b %BBSC_CHECK_EXIT%
