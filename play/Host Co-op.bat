@echo off
cd /d "%~dp0"
echo Hosting Robinsons Requiem co-op on port 27531.
echo Start or load your game; player 2 joins as soon as you are in the game.
RRQ-Coop.exe --host ..\game
