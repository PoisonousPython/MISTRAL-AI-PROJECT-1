@echo off
title Thermal Governor
powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0governor-wizard.ps1"
pause
