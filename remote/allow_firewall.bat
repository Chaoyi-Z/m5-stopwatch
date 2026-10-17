@echo off
:: Run once as administrator: lets the iPhone reach UxPlay (AirPlay mirroring).
net session >nul 2>&1 || (echo Right-click this file and choose "Run as administrator". & pause & exit /b 1)
netsh advfirewall firewall add rule name="UxPlay AirPlay receiver" dir=in action=allow program="C:\msys64\ucrt64\bin\uxplay.exe" enable=yes profile=any
pause
