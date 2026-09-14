@echo off
REM Supervisor for the Claude Stopwatch bridge: keeps it running and auto-restarts
REM it if it ever exits or crashes. Invoked hidden by the scheduled task's .vbs.
REM The log is truncated once at task start, then appended across restarts.
cd /d "D:\AI\Playground\Stopwatch\bridge"
type nul > "D:\AI\Playground\Stopwatch\bridge\bridge.log"
:loop
"C:\Python313\python.exe" -u bridge.py >> "D:\AI\Playground\Stopwatch\bridge\bridge.log" 2>&1
timeout /t 5 /nobreak >nul
goto loop
