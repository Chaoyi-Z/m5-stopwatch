Dim oShell
Set oShell = CreateObject("WScript.Shell")
' Launch bridge.py hidden via PowerShell; logs go to bridge.log / bridge_err.log
oShell.Run "powershell.exe -NonInteractive -WindowStyle Hidden -Command " & _
    "Start-Process 'C:\Python313\python.exe' " & _
    "-ArgumentList '-u','d:\AI\Playground\Stopwatch\bridge\bridge.py' " & _
    "-WorkingDirectory 'd:\AI\Playground\Stopwatch\bridge' " & _
    "-WindowStyle Hidden " & _
    "-RedirectStandardOutput 'd:\AI\Playground\Stopwatch\bridge\bridge.log' " & _
    "-RedirectStandardError 'd:\AI\Playground\Stopwatch\bridge\bridge_err.log'", 0, False
Set oShell = Nothing
