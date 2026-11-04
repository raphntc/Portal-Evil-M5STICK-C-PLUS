@echo off
REM ============================================================
REM  Portal Evil HUEBR - one-click firmware build (Windows)
REM  Double-click this file to compile the .bin for the
REM  M5StickC Plus. Output goes to the  firmware\  folder.
REM  Requires arduino-cli on PATH (see README).
REM ============================================================

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1"

echo.
echo Press any key to close...
pause >nul
