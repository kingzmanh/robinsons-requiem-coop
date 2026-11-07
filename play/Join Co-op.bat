@echo off
cd /d "%~dp0"
set /p HOSTIP=Host IP address: 
RRQ-Coop.exe --join %HOSTIP% ..\game
