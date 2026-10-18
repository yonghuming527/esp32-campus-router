# ESP32-S3 Dr.COM 校园网路由器

这是一个面向 ESP32-S3 的校园网共享路由固件。ESP32 连接开放的 2.4 GHz 校园 Wi-Fi，完成 Dr.COM Eportal 4.2.1 认证，再通过 SoftAP 与 NAPT 为个人设备提供网络。

本项目改编自 [moqin3380/esp32-campus-router](https://github.com/moqin3380/esp32-campus-router)，保留其 AP/STA 共存和 NAPT 思路，并针对实际 Dr.COM 门户、连接稳定性和 UDP 多服务器通信进行了适配。

## 功能

- Dr.COM Eportal 4.2.1 登录、在线状态检查与定时续期
- 开放校园 Wi-Fi 的自动扫描、同名 AP 信号优先选择与断线重连
- ESP32-S3 SoftAP，最多 4 个客户端共享网络
- NAPT 转发状态自检与异常恢复
- HT20、关闭 Wi-Fi 省电、Wi-Fi 缓冲配置等吞吐和稳定性调整
- USB 串口状态、局域网和外网诊断命令
- 修复同一 UDP socket 访问多个服务器时映射互相覆盖的问题

## 硬件和环境

- ESP32-S3，测试板为 8 MB Flash、8 MB OPI PSRAM
- Arduino ESP32 Core 3.2.0
- Arduino CLI
- 2.4 GHz 校园 Wi-Fi；ESP32-S3 不支持 5 GHz

当前构建配置：

```text
esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc
```

## 配置

复制私密配置模板：

```powershell
Copy-Item .\ESP32_S3\config_private.example.h .\ESP32_S3\config_private.h
```

然后填写校园网 SSID、门户地址、账号、密码和热点密码。`config_private.h` 已加入 `.gitignore`，不要把真实凭据提交到仓库。

本适配使用以下配置字段：

| 字段 | 含义 |
| --- | --- |
| `CAMPUS_SSID` / `CAMPUS_PASS` | 上游校园 Wi-Fi；开放网络密码留空 |
| `PORTAL` | 门户根地址 |
| `DRCOM_API` | Eportal API 地址，常见端口为 801 |
| `DRCOM_AC_IP` | 门户登录参数中的接入控制器地址 |
| `USERNAME` / `PASSWORD` | 校园网账号和密码 |
| `AP_SSID` / `AP_PASS` | ESP32 热点名和密码 |
| `AP_DNS` | 下发给热点客户端的可用 DNS |

门户字段随学校部署而不同。请通过本校登录页和前端请求确认，不要复制其他校园网的地址或账号格式。

## 编译与烧录

PowerShell：

```powershell
.\build.ps1
.\upload.ps1 -Port COM8
```

如果 `arduino-cli` 不在 `PATH` 中，可以显式指定：

```powershell
.\build.ps1 -Cli 'C:\path\to\arduino-cli.exe'
.\upload.ps1 -Port COM8 -Cli 'C:\path\to\arduino-cli.exe'
```

如果需要自定义 Arduino CLI 配置，再传入 `-ConfigFile`。构建产物位于 `build/`，不会提交到 Git。

串口监控示例：

```powershell
python .\monitor.py --port COM8 --seconds 60 --output serial.log
```

## 串口诊断

以换行结尾发送以下命令：

- `STATUS`：显示 STA/AP、信号、带宽、省电和客户端状态
- `LAN`：临时开放 `http://192.168.4.1:8080/download`，用于局域网吞吐测试，90 秒后关闭
- `WAN`：由 ESP32 发起有限大小的外网下载诊断

串口不会输出账号、密码或包含密码的登录 URL。

## UDP NAPT 修复

随 Arduino ESP32 Core 3.2.0 提供的 lwIP NAPT 实现，原先仅按协议、客户端 IP 和客户端端口查找 UDP 出站映射。同一 UDP socket 快速访问多个服务器时，后一个服务器会覆盖前一个服务器；入站回复仍要求匹配服务器地址和端口，因此早先服务器的回复会被丢弃。

`ESP32_S3/ip4_napt_udp_fix.c` 将 UDP 出站查找改为完整五元组，为不同服务器保留独立映射。入站来源校验、TCP、ICMP、表容量和超时均保持不变。

修复前的对照测试中，共享 socket 只收到 12/24 个回复；修复后复测为 24/24。普通 HTTP、HTTPS、DNS 和热点网关延迟测试也通过。源码来源、校验值和 SDK 升级要求见 [upstream/NAPT-PROVENANCE.md](upstream/NAPT-PROVENANCE.md)。

该文件与 ESP-IDF 5.4.1 / esp-lwIP 的特定提交绑定。升级 Arduino ESP32 Core 或 ESP-IDF 前，必须重新核对上游源码并复测链接映射和 UDP 行为。

## 测试结果

在单客户端、校园 2.4 GHz 环境下的短时实测：

| 项目 | 结果 |
| --- | --- |
| 2 MB HTTP 下载 | 约 9.2 Mbps |
| 5 MB HTTP 下载 | 约 7.7–8.9 Mbps |
| HTTPS 下载 | 约 8.4 Mbps |
| 合成数据上传 | 约 1.6 Mbps |
| UDP 双服务器共享 socket | 24/24 回复 |
| 热点网关 255 次探测 | 0 丢包，平均约 3.6 ms |

这些数据受校园网负载、AP 信号、射频干扰和客户端数量影响，只用于说明测试条件下的行为。

## 限制

- ESP32-S3 只有一套 2.4 GHz 射频，STA 与 AP 需要时分复用
- 多个热点客户端共享同一上游带宽；游戏、语音和大文件传输会互相影响
- 固件只适配所述 Dr.COM 门户流程，不适用于 802.1X、SRun 或其他认证系统
- 校园网策略变化、并发账号限制或门户参数变化可能导致认证失败
- 项目没有提供 VPN、流量整形、家长控制或商用路由器级可靠性

请只在本人有权使用并获准共享的网络中部署，遵守所在校园网的管理规定。

## 仓库内容

```text
ESP32_S3/
  ESP32_S3.ino
  DrcomAuth.h
  RouterDiagnostics.h
  ip4_napt_udp_fix.c
  config_private.example.h
upstream/
  NAPT-PROVENANCE.md
  ip4_napt-fa4dffdf.c.txt
  ip4_napt-udp-five-tuple.patch
build.ps1
upload.ps1
monitor.py
```

上游项目当前未声明仓库许可证；本仓库保留来源说明。vendored lwIP 文件内保留其原始 BSD 许可声明。
