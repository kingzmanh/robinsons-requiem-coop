@echo off
cd /d "%~dp0"
set /p HOSTIP=Host IP address: 
RRQ.exe --join %HOSTIP% ..\game
