@echo off
rem DOORS OS - Copyright (c) 2026 Joel Damaso ^<jammidee@gmail.com^>, Philippines.
rem Builds DOORS (64-bit) in WSL and boots it in QEMU, drawn right in this console.
rem Type DOORS commands here (try: help, vols, dir F:\).
rem Quit: press Alt+2 (or Esc then 2) for the QEMU monitor, type quit, press Enter.
rem Serial log: port64\build\serial-run.log. Delete port64\build\disk.img for a fresh disk.
title DOORS OS
mode con: cols=80 lines=25 >nul
echo Building and booting DOORS OS...
wsl -d Ubuntu-22.04 --cd "%~dp0port64" -- make -s console
echo.
echo DOORS has stopped.
pause
