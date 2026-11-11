# PrintHub

PrintHub 是一个仅支持 Windows 的网络打印服务，可将网络客户端提交的 RAW 或 LPD
打印任务转发到本机或已连接的 Windows 打印机。

项目包含：

- `PrintHub.exe`：Windows 服务程序，提供 RAW 和 LPD 网络打印。
- JSON 配置文件：维护打印机映射、监听地址、端口、日志和线程设置。
- 可选的 SNMP PPM MIB 发现功能，用于 Windows 添加标准 TCP/IP 打印机端口。

## 下载

从 [GitHub Releases](https://github.com/hvvvvvvv/PrintHub/releases/latest) 下载与系统架构相符的发布包：

- `PrintHub-<version>-windows-x86.zip`
- `PrintHub-<version>-windows-x64.zip`

每个 ZIP 包均包含：

- `PrintHub.exe`
- `config.example.json`
- `LICENSE`

x86 版本以 Windows 7 SP1 x86 为最低目标系统。请仅在受信任网络中部署，并按实际
使用的端口配置 Windows 防火墙。

## 快速开始

1. 解压对应架构的发布包。
2. 将 `config.example.json` 复制为 `config.json`，按
   [配置文件说明](docs/configuration.md) 编辑。将示例的 `Printer1` 替换为本机实际
   安装或已连接的 Windows 打印机名称，并设置对应的 LPR 队列名和可选 RAW 端口。
3. 在解压目录打开管理员 PowerShell，安装并启动服务：

```powershell
.\PrintHub.exe --service install
.\PrintHub.exe --service start
```

默认读取 `PrintHub.exe` 所在目录的 `config.json`。使用其他配置文件时，
将安装命令改为：

```powershell
.\PrintHub.exe -c "C:\PrintHub\office.json" --service install
```

`-c` 与 `--config` 等价；显式相对路径按命令执行时的当前工作目录解析，安装时将
配置文件的绝对路径写入服务 `ImagePath`。后续启动服务沿用已安装的配置路径。

修改该 JSON 后重启服务即可生效：

```powershell
.\PrintHub.exe --service restart
```

停止服务或卸载服务时，分别使用：

```powershell
.\PrintHub.exe --service stop
.\PrintHub.exe --service uninstall
```

查看用法和版本：

```powershell
.\PrintHub.exe --help
.\PrintHub.exe --version
```

不带 `--service` 时在前台运行，例如 `.\PrintHub.exe -c .\config.json`。
日志位于可执行文件旁的 `logs` 目录。已有旧版服务需先停止、卸载，再按新配置
安装并启动；具体步骤见[旧版迁移](docs/configuration.md#旧版迁移)。

## 配置字段

配置文件使用 UTF-8 JSON，最大 1 MiB。主要字段如下：

| 字段 | 用途与默认值 |
| --- | --- |
| `printerMap` | 默认 `[]`；每条映射必填 Windows 打印机名 `name` 和 LPR 队列名 `lprQueue`。 |
| `printerMap[].rawServer` | 可选；添加后同时提供 RAW 打印，默认地址 `0.0.0.0`、TCP 端口 `9100`。 |
| `lpdServer` | LPD 监听设置，默认地址 `0.0.0.0`、TCP 端口 `515`；映射非空时启动。 |
| `logLevel` | 日志级别，默认 `"info"`。 |
| `maxPendingClients` | 每个监听器等待客户端的上限，默认 `256`。 |
| `minThreads.raw` / `minThreads.lpd` | RAW / LPD 最小线程数，均默认为 `1`。 |
| `ppmDisabled` | 默认 `false`，启用 SNMP PPM；设为 `true` 可关闭。 |

`printerMap` 为空时不启动任何监听器、跳过 PPM，进程仍正常运行。每条映射均提供
LPR；省略 `rawServer` 可只提供 LPR。完整字段、默认值、端口冲突规则和编码限制见
[配置文件说明](docs/configuration.md)。

## SNMP PPM 发现

PrintHub 通过 `printerMap` 自动向 Windows 标准 TCP/IP 端口监视器公布打印机
及其 RAW/LPR 参数。`ppmDisabled` 缺省为 `false`，即默认开启；设置
`"ppmDisabled": true` 可关闭。PPM 启用且映射非空时，所有 LPD/RAW 监听器必须使用
同一个归一化地址；发现服务监听该地址（默认 `0.0.0.0`）的 UDP `161`，需放通该端口。
多地址部署须显式关闭 PPM。PPM 启动失败会记录 error 日志，打印服务仍可启动。

相同映射的发现索引在重启和映射重排后保持一致，不持久化索引。PPM 启用且映射
非空时，LPD 必须使用 TCP `515`；LPR 队列名须为 `1` 至 `32` 个可打印 ASCII
字符（`0x21`–`0x7e`），不含空白和 `=`。PPM 还会从后端 Windows 队列
尽力读取并缓存状态，并优先通过 Windows Bidi 自动读取真实 IEEE-1284 Device ID。
采集由 PrintHub 进程内的后台线程完成，发现和打印均不等待采集。新添加的 Windows
TCP/IP 端口默认不启用 SNMP 状态轮询；升级不会改写旧端口的设置。
Bidi 未返回有效非空 ID 时，仅对未脱机、由微软 USB 端口监视器管理且设备映射唯一的
本地队列尝试只读 USB 备用查询；失败时保留空身份，不伪造型号。获取阶段与错误码
记录在 info 日志，真实 USB 正向获取仍需实机验证。客户端匹配驱动取决于真实 ID
和已安装驱动。

完整的发现目录、客户端设置和诊断方式见
[Windows PPM 发现与部署说明](docs/windows-snmp-ppm.md)。

## 构建

构建需要：

- Windows
- Visual Studio 2022 或 Build Tools 2022
- “使用 C++ 的桌面开发”工作负载
- CMake 3.21 或更高版本
- Git（首次获取 vcpkg 依赖时需要）

构建过程中 vcpkg 会自动获取 `spdlog` 与 `nlohmann-json` 依赖。

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

- [配置文件与服务命令](docs/configuration.md)
- [Windows LPR 兼容性验证](docs/windows-lpr-compatibility.md)
- [Windows SNMP PPM 发现](docs/windows-snmp-ppm.md)
