@echo off
chcp 65001 >nul
set /p IP=请输入 iPhone 的 Wi-Fi IP (例如 192.168.1.20): 
echo.
echo 请先确保：OBS 音频源选择 监听和输出，监听设备选择 CABLE Input；
echo 已安装 VB-CABLE，Windows 上 ffmpeg 可以运行。
echo 你在 Filza 里应已创建 VCamLiveBridge.enable 和 VCamLiveBridge.pc-ip 文件。
echo.
ffmpeg -hide_banner -loglevel warning -f dshow -audio_buffer_size 50 -i audio="CABLE Output (VB-Audio Virtual Cable)" -ac 1 -ar 48000 -c:a pcm_s16le -f s16le "udp://%IP%:39876?pkt_size=1920"
pause
