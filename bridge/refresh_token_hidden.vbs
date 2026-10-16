' Launch refresh_token.cmd with no visible console window (window style 0).
' Used by the ClaudeTokenRefresh scheduled task so the 6-hourly token refresh
' doesn't flash a command prompt.
CreateObject("WScript.Shell").Run """D:\AI\Playground\Stopwatch\bridge\refresh_token.cmd""", 0, False
