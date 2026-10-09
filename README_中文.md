# VCam LIVE Audio Bridge v0.3 — 直播音频替换实验版

**这是带有实际音频注入逻辑的测试工程，不是已被证实修复 TikTok LIVE 的正式插件。** 原版 VCam 1.1.0 视频功能保留不变。请只在备用 iPhone 和合规的实时直播测试中使用。

## 目标

- iPhone 8 Plus (A11)、iPhone SE 2 (A13)
- iOS 15–16.6.1 / Dopamine Rootless 的**目标编译范围**；未经所有版本实机验证
- TikTok 包名 `com.zhiliaoapp.musically`；其他地区包名需要修改 `VCamLiveBridge.plist`
- 正常视频继续由**原版 VCam 1.1.0**从 OBS 接收
- 新的 TikTok App 内插件通过 UDP 接收电脑发来的 **48,000 Hz、单声道、16 位、小端 PCM** 音频，在 AudioUnitRender(Bus 1) 返回后尝试用它替换麦克风数据
- **没有网络音频超过 1.5 秒时自动恢复真实麦克风**。UDP 音频短暂断续则填充静音，避免突然泄露实体麦克风音轨。

## 先决条件

- 原版 VCam 1.1.0 已安装且能在 TikTok 普通拍摄中播放 OBS 画面和原声。
- 从 Sileo 卸载之前的 `VCam LIVE Audio Diagnostics v0.2`（包名 `com.vcam.liveprobe`），避免两个 Hook 同时作用于一个函数。
- Windows 电脑安装 **FFmpeg**，并确保在命令提示符运行 `ffmpeg -version` 有输出。
- 安装 **VB-CABLE 虚拟声卡**，官方下载：https://vb-audio.com/Cable/ 。安装后重启 Windows。
- 手机和电脑在同一个局域网，确保能通过 UDP 互相通信。某些 Wi-Fi 或防火墙会阻断。

## 1. 编译生成 DEB

将此 ZIP 解压，把**解压后的内容**（`Makefile`、`Tweak.c`、`control`、`VCamLiveBridge.plist`、`.github/workflows/build.yml` 等）全部上传到 GitHub 仓库**根目录**。不要再套一层文件夹！

进入 GitHub → Actions → `Build VCam LIVE Audio Bridge v0.3` → `Run workflow` → 等待成功，在构建任务底部 Artifacts 下载 `.deb`。也可以在已配好 Theos 和 iOS SDK 的 Mac 上执行：

```bash
export THEOS="$HOME/theos"
make clean package FINALPACKAGE=1
```

**目前只验证了跨平台 C 语法和 PCM 缓冲转换单元测试，未在 iOS SDK/真机环境完成编译或运行测试。** 编译失败请保留 GitHub Actions 完整日志。

## 2. 安装

使用 Sileo / Filza 安装编译出的 `com.vcam.livebridge` 测试 DEB，按要求 Respring。原版 `com.vcam` 保持安装。重启 TikTok App。

**默认不替换音频**：需要手动在 TikTok `tmp` 下创建两个配置文件才能启用。

## 3. Filza 里开启音频桥接

Filza → Apps Manager → TikTok → Data Container → `tmp`：

1. 创建 **`VCamLiveBridge.pc-ip`**（纯文本文件），内容只有电脑当前局域网 IPv4 地址，例如 `192.168.1.10`，最后可以换行，但**不要填写 iPhone 的 IP**。
2. 创建空文件 **`VCamLiveBridge.enable`**，作为启用开关。
3. 完全关闭 TikTok 后重新打开。

*电脑 IPv4 在 Windows 命令提示符输入 `ipconfig` 查看。手机 Wi-Fi IPv4 在 设置 → Wi-Fi → 点击连接的无线网络旁边的 `i` 查看。*

注意，`VCamLiveBridge.pc-ip` 内容应是 IP 文本，文件名不要变成 `...pc-ip.txt`。

## 4. 设置电脑 OBS 音频

1. OBS → 设置 → 音频 → 高级 → **监听设备**选 `CABLE Input (VB-Audio Virtual Cable)`。
2. OBS → 音频混音器 → 需要输出声音的来源右键 → **高级音频属性** → **音频监控**设 `监听并输出`。请防止 OBS 本身出现重复采集或回声。
3. 保留你之前已经能正常推画面的 OBS FFmpeg 自定义输出；这一步不改视频推流。
4. 解压后的 `windows` 文件夹内双击 `Windows_查看音频设备.bat`，确认 ffmpeg 能看到 `CABLE Output (VB-Audio Virtual Cable)`。
5. 双击 **`Windows_发送OBS声音.bat`**，按提示输入 **iPhone 的 Wi-Fi IP**。脚本用 ffmpeg 从 CABLE Output 采集 OBS 监听音频，编码成原始 PCM，持续发送到 `udp://iPhoneIP:39876`。

如果 Windows 上设备名略有差异，请按 `Windows_查看音频设备.bat` 显示的设备名修改发送脚本中 `audio="..."` 的内容。

## 5. 测试 TikTok LIVE

1. 原版 VCam 和电脑 OBS 的**视频流先启动**。
2. TikTok 打开后，使用**真实实时内容**进行简短、符合平台规则的 LIVE 测试。
3. 用另一个设备从观众端判断是否听到 OBS 音频。不要以主播预览无声判断故障。
4. 打开 Filza → TikTok Data Container → `tmp` → `VCamLiveBridge.log`。

重点看下列字段（所有数值均是累计值）：

| 日志字段 | 含义 |
| --- | --- |
| `enabled=1 sourceIPConfigured=1` | 两个配置文件已正确识别 |
| `udpPackets` 不断增长 | 电脑 PCM 已被 TikTok 内插件收到 |
| `injected` 不断增长 | 插件已经尝试覆盖 AudioUnitRender 缓冲区 |
| `rate=48000 bits=16/32` | 音频缓冲格式已识别（对应源以 48 kHz 发送） |
| `unsupported` 不断增长 | 目标 AudioUnit PCM 格式不受本测试版支持 |
| `underflows` 增长很快 | 电脑音频发送不及时；换更稳定的网络、降低 CPU 占用 |
| `socketFailures>0` | 可能无法在 TikTok 进程绑定 UDP 39876 端口 |
| `udpPackets>0 injected=0` | UDP 收到但未进入可替换的音频缓冲路径，需后续修正 |
| `injected>0` 但观众依然听到手机真实麦克风 | Bus 1 可能不是最终直播上传音轨，不能认为问题已解决 |

## 6. 随时停止及回退

- **立即停用（最多约 1 秒生效）：**删除 `VCamLiveBridge.enable` 文件，然后可停止 Windows 发送脚本。插件只观察，不替换声音。
- 如果未启用开关、配置不正确、OBS 停止发送超过 1.5 秒，代码会保留真实麦克风输入。
- **完整卸载：**Sileo 卸载 `com.vcam.livebridge`，必要时 Respring / 强制重启 TikTok。原版 `VCam 1.1.0` 不受影响。
- 如果 TikTok 闪退、录音异常、手机发热或系统媒体服务不稳定，请立即停止测试并卸载桥接插件；不要在正式直播中尝试。

## 技术边界

- 使用独立音频流，不等于复用了 VCam 原版的解码音轨。因此音画同步还需单独调整，不能承诺 0 延迟。
- AudioUnitRender Bus 1 在日志里有调用，不代表它一定是 TikTok LIVE 最终编码器的输入；需要实际观众端验证。
- 收包端为了降低误接收，只接受 `VCamLiveBridge.pc-ip` 文件指定的电脑 IPv4，但 UDP 没有加密或密码认证，请只在可信局域网测试。
- TikTok App 可能没有成功绑定 UDP 接收端口，或者有不同的音频采集路径，日志会反映这些情况。
- 不用于把预录视频或音频伪装成实时直播；应遵守直播平台规则。
