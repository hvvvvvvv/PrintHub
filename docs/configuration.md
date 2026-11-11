# PrintHub 配置文件与服务命令

PrintHub 从一个 JSON 文件读取全部打印服务配置，包括打印机映射、监听地址、
端口、日志、线程和 PPM 发现设置。服务启动时读取配置；修改后重启服务生效。

## 文件位置与路径

默认配置路径是 `<PrintHub.exe 所在目录>\config.json`，与启动命令的工作目录无关。
首次使用时，将发布包中的 `config.example.json` 复制为 `config.json` 并编辑。

可以使用 `-c PATH`、`--config PATH`、`-c=PATH` 或 `--config=PATH` 指定其他文件。
显式指定的相对路径以进程当前工作目录为基准解析为绝对路径；包含空格的路径应加
双引号。配置文件路径支持 Unicode，同一命令重复指定配置路径会报错。

执行 `--service install` 时，安装程序将解析后的配置文件绝对路径写入 Windows
服务的 `ImagePath`。随后由服务管理器启动时，使用已保存的路径，不受服务工作目录
影响。修改文件内容后只需重启；要改用另一份文件，需停止、卸载，再用新路径安装。

## 格式与错误

- 文件必须为 UTF-8，可带 UTF-8 BOM，最大 `1 MiB`（`1,048,576` 字节）。
- 使用标准 JSON，不支持注释；根节点必须为对象。
- 任意层级的未知字段、类型错误、越界数值、非法 JSON 都会导致配置读取失败，
  阻止服务启动。整数不能写成字符串或浮点数，`null` 不能代替省略字段。
- 错误包含文件、字段路径和原因，例如：

```text
config.json: printerMap[0].rawServer.port: expected integer 1..65535
```

只有下表中的字段受支持；没有顶层 `listenAddress` 或 `version` 字段。
省略可选字段时使用默认值；`printerMap` 中每条映射的 `name` 和 `lprQueue` 必填。

## 字段表

字段路径中的 `[]` 表示 `printerMap` 数组中的一条映射。

| 字段 | 类型 | 默认值 | 约束与含义 |
| --- | --- | --- | --- |
| `logLevel` | 字符串 | `"info"` | `trace`、`debug`、`info`、`warn`、`error`、`critical`、`off`，大小写不敏感。 |
| `maxPendingClients` | 整数 | `256` | `1`–`65535`，每个监听器等待客户端的上限。 |
| `lpdServer` | 对象 | 使用子字段默认值 | 只包含 `listenAddress`、`port`。 |
| `lpdServer.listenAddress` | 字符串 | `"0.0.0.0"` | 合法 IPv4 地址；`0.0.0.0` 表示所有 IPv4 接口。 |
| `lpdServer.port` | 整数 | `515` | `1`–`65535`；PPM 启用且 `printerMap` 非空时必须为 `515`。 |
| `minThreads` | 对象 | 使用子字段默认值 | 只包含 `raw`、`lpd`。 |
| `minThreads.raw` | 整数 | `1` | `1`–`65535`，RAW 最小线程数。 |
| `minThreads.lpd` | 整数 | `1` | `1`–`65535`，LPD 最小线程数。 |
| `printerMap` | 对象数组 | `[]` | 可为空；每个对象只能包含 `name`、`lprQueue`、`rawServer`。 |
| `printerMap[].name` | 字符串 | 无，必填 | 非空，指定本机已安装或已连接的 Windows 打印机名称；允许多条映射使用同一名称。 |
| `printerMap[].lprQueue` | 字符串 | 无，必填 | 非空，不得包含 CR/LF，按 ASCII 字母大小写不敏感的规则全局唯一；PPM 启用时另受下文限制。 |
| `printerMap[].rawServer` | 对象 | 省略时不启动该条 RAW 监听器 | 可选，只包含 `listenAddress`、`port`；写为 `{}` 时使用 RAW 子字段默认值。 |
| `printerMap[].rawServer.listenAddress` | 字符串 | `"0.0.0.0"` | 合法 IPv4 地址；每条 RAW 映射独立设置。 |
| `printerMap[].rawServer.port` | 整数 | `9100` | `1`–`65535`；RAW 端点必须唯一，且不得与其他监听器冲突。 |
| `ppmDisabled` | 布尔值 | `false` | `false` 启用 SNMP PPM，`true` 关闭；空映射始终跳过 PPM。 |

所有字符串均拒绝内嵌 NUL（包括 JSON 转义 `\u0000`）。打印机名和队列名还须满足
[编码限制](#编码限制)。

## 完整示例

以下内容与发布包中的 `config.example.json` 一致。将 `name` 的 `Printer1`
替换为实际 Windows 打印机名称；`lprQueue` 是客户端使用的队列名，可独立命名。
此示例提供 LPR/TCP `515`、RAW/TCP `9100` 和 SNMP/UDP `161`。

```json
{
  "logLevel": "info",
  "maxPendingClients": 256,
  "lpdServer": {
    "listenAddress": "0.0.0.0",
    "port": 515
  },
  "minThreads": {
    "raw": 1,
    "lpd": 1
  },
  "printerMap": [
    {
      "name": "Printer1",
      "lprQueue": "Printer1",
      "rawServer": {
        "listenAddress": "0.0.0.0",
        "port": 9100
      }
    }
  ],
  "ppmDisabled": false
}
```

示例中的单条映射是部署示例，不是 `printerMap` 的默认值；该字段缺省时为 `[]`。

## 映射与监听规则

`printerMap` 为空是合法配置，包括只写 `{}` 的情况。此时不启动任何监听器，
跳过 PPM，进程仍正常运行；不要求至少配置一个监听器。

映射非空时启动一个 LPD 监听器，使用 `lpdServer.listenAddress` 和
`lpdServer.port`。每条映射必须同时给出 `name` 和 `lprQueue`，即使客户端计划
只通过 RAW 打印，也必须配置 LPR 队列名。服务只接受显式配置的队列，不根据
Windows 打印机名称或共享名称推断目标。

省略某条映射的 `rawServer` 时，该条映射只提供 LPR；提供 `rawServer` 时，
还会启动对应的 RAW 监听器。可以用多条相同 `name` 的映射向同一台打印机提供
多个 LPR 队列或 RAW 端口，但所有 `lprQueue` 必须大小写不敏感唯一，所有 RAW
端点（规范化后的 IPv4 地址与端口）必须唯一。

监听地址按 IPv4 地址值归一化后比较。实际会启动的 RAW 与 LPD 监听器共同参与
冲突检查：端口相同，且地址相同或任一方为 `0.0.0.0`，即为冲突。例如
`0.0.0.0:9100` 与 `192.168.1.10:9100` 冲突；两个不同的具体地址可以复用同一
端口，但须关闭 PPM。LPD 只在 `printerMap` 非空时参与检查。RAW 或 LPD 绑定失败
会阻止服务启动。

PPM 启用且 `printerMap` 非空时，所有实际启动的监听器（LPD 与各 RAW）必须使用
同一个归一化 IPv4 地址；`0.0.0.0` 与具体地址不能混用。多地址部署必须显式设置
`"ppmDisabled": true`，关闭后仍需通过上述端点唯一性与冲突检查。

## PPM 发现

`ppmDisabled` 缺省为 `false`，PPM 默认启用，并根据 `printerMap` 自动生成
打印机及 RAW/LPR 端口目录。启用时须满足：

- 每个 `lprQueue` 为 `1`–`32` 个可打印 ASCII 字符（`0x21`–`0x7e`），
  不含空白和 `=`。
- `printerMap` 非空时，`lpdServer.port` 必须为 `515`。
- `printerMap` 非空时，LPD 与所有 RAW 监听器必须使用同一个归一化监听地址。
  可以全部使用 `0.0.0.0`，也可以全部使用同一个具体 IPv4 地址。

设置 `"ppmDisabled": true` 后不启动 PPM，上述 PPM 专用限制不再适用；队列名
仍须非空、不含 CR/LF、大小写不敏感唯一，并满足编码要求，LPD 端口仍须在
`1`–`65535`。已有多地址配置若保留默认开启的 PPM，将校验失败；需统一所有监听
地址，或显式关闭 PPM。

SNMP/PPM 绑定与打印监听器相同的地址（取 `lpdServer.listenAddress`，默认
`0.0.0.0`），UDP 端口固定为 `161`，确保公布的打印端口在 SNMP 应答接口上可达。
需要放通入站 UDP `161` 及实际打印使用的 TCP 端口。PPM 启动失败（例如 UDP
`161` 被占用）只记录 error 日志，不阻止打印服务启动；配置校验失败仍会阻止启动。
空 `printerMap` 始终跳过 PPM，即使未禁用 PPM 也不会监听 UDP `161`。

发现索引、Windows 客户端设置及后端状态采集见
[Windows SNMP PPM 发现](windows-snmp-ppm.md)。

## 编码限制

配置文件始终使用 UTF-8。打印机名、队列名读入进程后转换为系统 ANSI 代码页
（ACP），与 `OpenPrinterA` 和 PPM 使用的内部字符串一致；写回文件时从 ACP
转换回 UTF-8。无效编码或系统代码页无法表示的字符会报错，不会静默替换。

因此，Windows 打印机名称可以使用当前系统代码页支持的中文等字符；PPM 启用时，
LPR 队列名必须使用上述 ASCII 字符。后台打印机名称和 LPR 队列名是不同字段，
不需要相同。将配置移到使用不同系统代码页的机器时，应重新确认名称可表示且匹配
该机器上实际安装或已连接的打印机。

## 服务命令

支持的命令形式：

```text
PrintHub.exe [--config <path>]
PrintHub.exe [--config <path>] --service <install|uninstall|start|stop|restart>
PrintHub.exe --help
PrintHub.exe --version
```

在程序目录的管理员 PowerShell 中安装并启动默认配置：

```powershell
.\PrintHub.exe --service install
.\PrintHub.exe --service start
```

若需指定文件，将安装命令替换为以下形式；`-c` 与 `--config` 等价：

```powershell
.\PrintHub.exe -c "C:\PrintHub\office.json" --service install
```

服务使用安装时保存的配置路径。编辑该文件后重启，不需要重新安装：

```powershell
.\PrintHub.exe --service restart
```

停止及卸载：

```powershell
.\PrintHub.exe --service stop
.\PrintHub.exe --service uninstall
```

前台运行时省略 `--service`；下例显式路径相对于当前工作目录：

```powershell
.\PrintHub.exe --config .\config.json
```

查询用法和版本：

```powershell
.\PrintHub.exe --help
.\PrintHub.exe --version
```

`--service ACTION` 与 `--service=ACTION` 等价。缺少或使用未知 action 时会报错，
并列出 `install`、`uninstall`、`start`、`stop`、`restart`。命令用法错误退出码为
`1`；帮助和版本命令退出码为 `0`。`--version` 只输出一行 `1.0.2`。
日志保存在可执行文件旁的 `logs` 目录。

## 旧版迁移

旧的命令行配置及服务 `ImagePath` 参数不再受支持，没有兼容层或自动迁移。
迁移顺序为：停止旧服务 → 卸载旧服务 → 准备新配置 → 安装新服务 → 启动。

1. 在替换旧程序前，在旧程序目录的管理员 PowerShell 中执行
   `.\PrintHub.exe --service stop`，再执行 `.\PrintHub.exe --service uninstall`。
2. 部署新版本，将 `config.example.json` 复制为 `config.json`，按原部署填写
   `printerMap`、监听地址、端口及其他设置。每条映射都需要 `lprQueue`；同时确认
   默认开启的 PPM 是否符合部署需求。
3. 在新程序目录执行 `.\PrintHub.exe --service install`，或用 `-c` 指定文件，
   然后执行 `.\PrintHub.exe --service start`。

## 后续配置界面

该 JSON 是后续 Web UI 的唯一配置来源。配置模块已提供解析、序列化和文件读写
API；后续界面应通过这些 API 读写同一份文件，并遵循相同的校验规则。当前操作
方式为编辑 JSON 和使用上述服务命令。
