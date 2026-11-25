@echo off
setlocal EnableExtensions
title KameraGeulis - Hapus Kamera Virtual

set "SCRIPT_DIR=%~dp0"
set "DLL64=%SCRIPT_DIR%softcam.dll"
set "DLL32=%SCRIPT_DIR%win32\softcam.dll"

fltmc >nul 2>&1
if errorlevel 1 (
  echo Meminta izin Administrator ^(UAC^) untuk menghapus registrasi KameraGeulis Camera...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','\"\"%~f0\" %*\"' -Verb RunAs -Wait"
  exit /b %errorlevel%
)

echo Menghapus registrasi KameraGeulis Camera...
if exist "%DLL64%" (
  "%SystemRoot%\System32\regsvr32.exe" /u /s "%DLL64%"
)
if exist "%DLL32%" (
  "%SystemRoot%\SysWOW64\regsvr32.exe" /u /s "%DLL32%"
)

rem Pastikan sisa entri COM/DirectShow di registry bersih total (x64 + WOW6432Node)
reg delete "HKLM\SOFTWARE\Classes\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\{AEF3B972-5FA5-4647-9571-358EB472BC9E}" /f /reg:64 >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\{AEF3B972-5FA5-4647-9571-358EB472BC9E}" /f /reg:64 >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\Instance\{AEF3B972-5FA5-4647-9571-358EB472BC9E}" /f /reg:32 >nul 2>&1
reg delete "HKLM\SOFTWARE\Classes\CLSID\{AEF3B972-5FA5-4647-9571-358EB472BC9E}" /f /reg:32 >nul 2>&1

echo.
echo [SUKSES] Registrasi "KameraGeulis Camera" dan entri registry telah dibersihkan.
if /I "%~1" NEQ "--nopause" pause
exit /b 0
