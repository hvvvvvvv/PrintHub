# PrintHub

PrintHub 是一个仅支持 Windows 的网络打印服务，可将网络客户端提交的 RAW 或 LPD
打印任务转发到本机或已连接的 Windows 打印机。

项目包含：

- `PrintHub.exe`：Windows 服务程序，提供 RAW 和 LPD 网络打印。
- JSON 配置文件：维护打印机映射、监听地址、端口、日志和线程设置。
- 内置 Web 面板：通过 token 鉴权查看状态、日志和编辑配置，保存后热重载。
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
3. 在解压目录打开管理员 PowerShell，选择一种 token 方式前台启动。
   显式指定时，将下面的示例替换为自己的 token；长度须为 `16`–`256`，
   字符范围为 `0x21`–`0x7e`（可打印 ASCII，不含空格）：

   ```powershell
   .\PrintHub.exe -c .\config.json --web-token "ChangeMe-0123456789"
   ```

   也可以省略 `--web-token`，由程序每次启动生成 32 字节随机数，编码为 64 个小写
   十六进制字符。启动后在 `logs\PrintHub_YYYY-MM-DD.log` 中查找
   `Web UI access token generated for this start:`：

   ```powershell
   .\PrintHub.exe -c .\config.json
   ```

4. 需要作为 Windows 服务运行时，先退出前台进程，再安装并启动。使用固定 token：

   ```powershell
   .\PrintHub.exe -c .\config.json --web-token "ChangeMe-0123456789" --service install
   .\PrintHub.exe --service start
   ```

   使用每次启动随机生成的 token，则安装时省略该参数：

   ```powershell
   .\PrintHub.exe -c .\config.json --service install
   .\PrintHub.exe --service start
   ```

   前台运行与服务运行择一使用，避免争用打印和 Web 端口。
5. 在本机浏览器打开 <http://127.0.0.1:8323/>，首次输入本次启动日志中的 token
   （或 `--web-token` 指定的值）。
   面板将 token 保存在当前标签页的 `sessionStorage`，可查看状态卡、实时日志，
   编辑打印机映射并保存。

随机模式下，每次服务或进程重启都会更换 token，浏览器需重新输入；需要稳定值时显式
指定 `--web-token`。两种方式都会在每次启动时将当前 token 写入日志，即使
`logLevel` 高于 `info` 或为 `off`；配置热重载不会更换或重复打印 token。

默认读取 `PrintHub.exe` 所在目录的 `config.json`。使用其他配置文件时，
将安装命令改为：

```powershell
.\PrintHub.exe -c "C:\PrintHub\office.json" --service install
```

`-c` 与 `--config` 等价；显式相对路径按命令执行时的当前工作目录解析，安装时将
配置文件的绝对路径写入服务 `ImagePath`。后续启动服务沿用已安装的配置路径。
显式 Web 参数也会写入 `ImagePath`，其中显式 token 保存原始字符串；省略时
不保存 token，由服务每次启动时生成。token 会出现在启动日志中，显式指定时还会出现在
命令行和服务注册表配置中，应限制日志目录与服务配置的访问权限。

**热重载**：在面板保存配置会同步写回当前 `config.json` 并即时应用，不重启 Windows
服务，Web 面板保持可用；活动打印连接会中断，保存前会请求确认，`logLevel` 也会即时生效。
直接编辑 JSON 文件不会自动加载，仍需重启服务：

```powershell
.\PrintHub.exe --service restart
```

局域网访问需要显式绑定。例如，在 `C:\PrintHub` 中准备好配置和 token 后，前台运行：

```powershell
.\PrintHub.exe -c .\config.json --web-bind 0.0.0.0 --web-port 8323 --web-token "ChangeMe-0123456789"
```

客户端访问 `http://<服务器IPv4>:8323/`。要以服务运行，在安装命令中传入同样的 Web
参数，再执行 `--service start`；已有服务需停止、卸载后按新参数安装。
按需放通入站 TCP `8323` 并限制来源地址；防火墙命令和服务安装示例见
[Web 面板说明](docs/web-ui.md)。面板仅使用 HTTP，token 明文传输；仅在受信任网络
使用，局域网部署建议使用 VPN 或提供 TLS 的反向代理。

停止服务或卸载服务时，分别使用：

```powershell
.\PrintHub.exe --service stop
.\PrintHub.exe --service uninstall
```

查看用法和版本；`--help` 包含三个 Web 参数的默认值与局域网启动示例：

```powershell
.\PrintHub.exe --help
.\PrintHub.exe --version
```

不带 `--service` 时在前台运行，例如 `.\PrintHub.exe -c .\config.json`。
日志位于可执行文件旁的 `logs` 目录。已有旧版服务需先停止、卸载，再按新配置
安装并启动；具体步骤见[旧版迁移](docs/configuration.md#旧版迁移)。

## 命令行参数

```text
PrintHub.exe [--config <path>] [--web-bind <IPv4>] [--web-port <1-65535>] [--web-token <token>]
PrintHub.exe [--config <path>] [--web-bind <IPv4>] [--web-port <1-65535>] [--web-token <token>] --service <install|uninstall|start|stop|restart>
```

| 参数 | 说明与默认值 |
| --- | --- |
| `-c` / `--config <path>` | UTF-8 JSON 配置文件；默认 `<exe目录>\config.json`。 |
| `--web-bind <IPv4>` | Web 监听地址；默认 `127.0.0.1`，局域网访问需显式绑定服务器地址或 `0.0.0.0`。 |
| `--web-port <1-65535>` | Web TCP 端口；默认 `8323`，禁止 `0`。 |
| `--web-token <token>` | 16–256 个可打印 ASCII 字符（不含空格）；默认每次启动随机生成并打印到日志。 |
| `--service <action>` | `install`、`uninstall`、`start`、`stop`、`restart`；省略时在前台运行。 |
| `--help` | 显示用法、Web 默认参数和局域网示例后退出。 |
| `--version` | 显示版本后退出。 |

Web 参数支持空格和等号形式，例如 `--web-port=8323`；同一参数重复指定会报错。
Web 监听地址、端口和显式 token 来自启动命令行或服务 `ImagePath`，不属于 JSON
配置字段；省略 token 时在启动时随机生成，不能通过面板热修改。
完整说明见 [Web 面板与 HTTP API](docs/web-ui.md)。

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

`printerMap` 为空时不启动 RAW/LPD 打印监听器、跳过 PPM，进程仍正常运行，Web 面板可独立
启用。每条映射均提供 LPR；省略 `rawServer` 可只提供 LPR。完整字段、默认值、端口冲突规则和编码限制见
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

构建过程中自动获取 `spdlog`、`nlohmann-json` 和 `cpp-httplib` 依赖。

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
- [Web 面板、热重载与 HTTP API](docs/web-ui.md)
- [Windows LPR 兼容性验证](docs/windows-lpr-compatibility.md)
- [Windows SNMP PPM 发现](docs/windows-snmp-ppm.md)
