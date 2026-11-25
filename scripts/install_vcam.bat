@echo off
setlocal EnableExtensions
title KameraGeulis - Pasang Kamera Virtual

set "SCRIPT_DIR=%~dp0"
set "DLL64=%SCRIPT_DIR%softcam.dll"
set "DLL32=%SCRIPT_DIR%win32\softcam.dll"

if not exist "%DLL64%" (
  echo [ERROR] softcam.dll tidak ditemukan di "%SCRIPT_DIR%".
  echo Pastikan install_vcam.bat berada di folder yang sama dengan KameraGeulis.exe dan softcam.dll.
  if /I "%~1" NEQ "--nopause" pause
  exit /b 1
)

net session >nul 2>&1
if errorlevel 1 (
  echo Meminta izin Administrator ^(UAC^) untuk mendaftarkan KameraGeulis Camera...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath 'cmd.exe' -ArgumentList '/c','\"\"%~f0\" %*\"' -Verb RunAs -Wait"
  exit /b %errorlevel%
)

echo Mendaftarkan KameraGeulis Camera ^(x64^)...
"%SystemRoot%\System32\regsvr32.exe" /s "%DLL64%"
if errorlevel 1 (
  echo [GAGAL] regsvr32 x64 gagal ^(kode %errorlevel%^).
  if /I "%~1" NEQ "--nopause" pause
  exit /b %errorlevel%
)

if exist "%DLL32%" (
  echo Mendaftarkan KameraGeulis Camera ^(Win32 untuk aplikasi 32-bit seperti Discord^)...
  "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%DLL32%"
  if errorlevel 1 (
    echo [PERINGATAN] regsvr32 Win32 gagal ^(kode %errorlevel%^). Aplikasi 64-bit tetap berfungsi.
  )
) else (
  echo [INFO] win32\softcam.dll tidak ditemukan; hanya mendaftarkan x64.
)

reg query "HKLM\SOFTWARE\Classes\CLSID\{AEF3B972-5FA5-4647-9571-358EB472BC9E}\InprocServer32" /reg:64 >nul 2>&1
if errorlevel 1 (
  echo [GAGAL] Entri registry tidak ditemukan setelah registrasi.
  if /I "%~1" NEQ "--nopause" pause
  exit /b 2
)

echo.
echo [SUKSES] "KameraGeulis Camera" berhasil didaftarkan!
echo Jalankan KameraGeulis.exe tanpa hak admin dan aktifkan "Kamera Virtual" di tab Kamera.
if /I "%~1" NEQ "--nopause" pause
exit /b 0
