# PrintHub

PrintHub 是一个仅支持 Windows 的网络打印服务，可将网络客户端提交的 RAW 或 LPD
打印任务转发到本机或已连接的 Windows 打印机。

项目包含：

- `PrintHub.exe`：Windows 服务程序，提供 RAW 和 LPD 网络打印。
- `PrintHubManager.exe`：需要管理员权限的本机管理面板，用于维护打印机映射并安装、
  更新和管理服务。
- 可选的 SNMP 快速拒绝功能，用于改善 Windows 添加标准 TCP/IP 打印机端口时的发现体验。

## 下载

从 [GitHub Releases](https://github.com/hvvvvvvv/PrintHub/releases/latest) 下载与系统架构相符的发布包：

- `PrintHub-<version>-windows-x86.zip`
- `PrintHub-<version>-windows-x64.zip`

每个 ZIP 包均包含：

- `PrintHub.exe`
- `PrintHubManager.exe`
- `LICENSE`

x86 版本以 Windows 7 SP1 x86 为最低目标系统。请仅在受信任网络中部署，并按实际
使用的端口配置 Windows 防火墙。

## 快速开始

### 使用管理面板

1. 解压对应架构的发布包。
2. 以管理员身份运行 `PrintHubManager.exe`。
3. 在“RAW 端口映射”中添加“本地端口 → Windows 打印机”映射，或在“LPD 队列映射”中
   添加“队列名 → Windows 打印机”映射。
4. 点击“安装/更新并启动”。

管理面板会将服务配置写入 Windows 服务，并提供启动、停止、重启、卸载和日志目录打开功能。

### 使用命令行

在管理员 PowerShell 中，安装 RAW 打印服务：

```powershell
.\PrintHub.exe --raw-server "HP LaserJet:9100" --service install
.\PrintHub.exe --service start
```

安装 LPD 打印服务：

```powershell
.\PrintHub.exe --lpd-queue "office=HP LaserJet" --service install
.\PrintHub.exe --service start
```

- RAW 映射格式为 `打印机名称:端口`；可重复指定多个 `--raw-server`。
- LPD 映射格式为 `队列名=打印机名称`；仅接受已配置的队列。
- LPD 默认监听 TCP `515`；RAW 监听配置映射中的端口，例如 `9100`。

查看全部参数：

```powershell
.\PrintHub.exe --help
```

## SNMP 快速拒绝

Windows 的“添加标准 TCP/IP 打印机端口”向导可能会等待 SNMP 探测超时。可在服务配置中
添加 `--snmp-reject`，让 PrintHub 在 UDP `161` 上快速响应 SNMP v1/v2c `public` 探测请求。

该功能不模拟打印机。创建端口时仍应选择“通用网络卡”、使用 RAW 协议，并在创建后关闭
“SNMP 状态已启用”。

详见 [Windows SNMP 快速拒绝说明](docs/windows-snmp-reject.md)。

## 构建

构建需要：

- Windows
- Visual Studio 2022 或 Build Tools 2022
- “使用 C++ 的桌面开发”工作负载
- **C++ MFC for latest v143 build tools**
- CMake 3.21 或更高版本
- Git（首次获取 vcpkg 依赖时需要）

构建 x64 Release：

```powershell
.\scripts\build.cmd -Architecture x64 -Configuration Release
```

构建 x86 Release：

```powershell
.\scripts\build.cmd -Architecture x86 -Configuration Release
```

输出位于 `out\publish\<architecture>-release\`。

## 文档

- [服务管理面板](docs/service-manager.md)
- [Windows LPR 兼容性验证](docs/windows-lpr-compatibility.md)
- [Windows SNMP 快速拒绝](docs/windows-snmp-reject.md)
