' Launch the Claude Stopwatch bridge hidden (no console window).
' Invoked by the "ClaudeStopwatchBridge" scheduled task (and survives Claude Code restarts).
CreateObject("WScript.Shell").Run "cmd /c ""D:\AI\Playground\Stopwatch\bridge\run_bridge.bat""", 0, False
