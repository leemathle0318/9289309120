@echo off
chcp 65001 >nul
ffmpeg -hide_banner -list_devices true -f dshow -i dummy
pause
