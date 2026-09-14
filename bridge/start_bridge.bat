@echo off
REM Launches bridge.py silently (no console window) and logs output to bridge.log
REM Double-click to start, or let Task Scheduler run it on login.
cd /d "%~dp0"
start "" /B "C:\Python313\pythonw.exe" -u bridge.py > bridge.log 2>&1
