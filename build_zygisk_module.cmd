@echo off
setlocal
powershell -ExecutionPolicy Bypass -File "%~dp0build_zygisk_module.ps1" %*
if errorlevel 1 (
  echo [build] Failed with exit code %errorlevel%
  exit /b %errorlevel%
)
echo [build] Done
