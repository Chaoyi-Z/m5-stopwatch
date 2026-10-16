@echo off
rem Token-freshness checker. Runs refresh_token.py, which calls the official
rem `claude` CLI to refresh the OAuth access token only when it's expiring. The
rem ClaudeTokenRefresh scheduled task runs this every ~15 min via the hidden .vbs,
rem so the bridge's Claude usage never goes stale.
cd /d "%~dp0"
python "D:\AI\Playground\Stopwatch\bridge\refresh_token.py" >nul 2>&1
