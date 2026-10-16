@echo off
rem Launch the AI Usage desktop widget (no console window).
cd /d "%~dp0"
start "" pythonw.exe usage_widget.py
