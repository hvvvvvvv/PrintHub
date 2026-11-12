# Windows 标准 TCP/IP 端口发现：PPM MIB

PrintHub 根据 JSON 配置文件的 `printerMap`，将 Windows 打印机和 RAW/LPR 端口公布为只读
Printer Port Monitor MIB（PPM MIB）。Windows 标准 TCP/IP 端口监视器可以读取
这份目录，并在有多个可用端口时让用户选择，再填入对应的协议和目标端口。

`ppmDisabled` 缺省为 `false`，即默认开启 PPM；设置 `"ppmDisabled": true`
可关闭。映射非空且 PPM 启用时，SNMP 监听与打印监听器相同的地址（默认
`0.0.0.0`），UDP 端口为 `161`。配置字段和完整默认值见[配置文件说明](configuration.md)。

## 启动和安装

将下面任一示例保存为 UTF-8 的 `config.json`，或将相应字段写入已有配置。
打印机名称必须替换为 PrintHub 主机上实际安装或已连接的 Windows 打印机名称。
省略的字段采用默认值，包括 LPD 地址 `0.0.0.0` 和 TCP 端口 `515`。

同一 IP 提供两台打印机，每条映射同时提供 RAW 与 LPR：

```json
{
  "printerMap": [
    {
      "name": "Office Printer",
      "lprQueue": "office",
      "rawServer": {
        "listenAddress": "0.0.0.0",
        "port": 9100
      }
    },
    {
      "name": "Reports Printer",
      "lprQueue": "reports",
      "rawServer": {
        "listenAddress": "0.0.0.0",
        "port": 9101
      }
    }
  ],
  "ppmDisabled": false
}
```

仅提供 LPR 时省略 `rawServer`；每条映射的 `lprQueue` 始终必填：

```json
{
  "printerMap": [
    {
      "name": "Office Printer",
      "lprQueue": "office"
    },
    {
      "name": "Reports Printer",
      "lprQueue": "reports"
    }
  ],
  "ppmDisabled": false
}
```

在管理员 PowerShell 中安装并启动服务：

```powershell
.\PrintHub.exe --config .\config.json --service install
.\PrintHub.exe --service start
```

安装时会将配置文件的绝对路径写入服务 `ImagePath`。之后修改该文件，再执行
`.\PrintHub.exe --service restart` 即可更新映射和 PPM 设置，无需重新安装。

从使用旧命令行配置的版本升级时，需先停止、卸载旧服务，准备新 JSON 配置后再
安装并启动；步骤见[旧版迁移](configuration.md#旧版迁移)。

## 监听和 Windows 端口设置

LPD 使用 `lpdServer.listenAddress`，每个 RAW 监听器使用各自的
`printerMap[].rawServer.listenAddress`，缺省均为 `0.0.0.0`。PPM 启用且映射非空时，
所有实际启动的 LPD/RAW 监听器必须使用同一个归一化 IPv4 地址，不能混用
`0.0.0.0` 与具体地址。SNMP 绑定该共同地址的 UDP `161`，确保目录中公布的打印
端口在 SNMP 应答接口上可达。多地址部署必须设置 `"ppmDisabled": true`，并满足
配置中的端点唯一性与冲突规则。`printerMap` 为空时不启动任何监听器、跳过 PPM，
进程仍正常运行。

若 UDP `161` 被 Windows SNMP Service 或其他程序占用，PPM 启动失败只会记录
error 日志，不阻止打印服务启动；RAW/LPD 绑定失败仍会阻止启动。

Windows 防火墙需要放通入站 UDP `161`，以及实际使用的 RAW TCP 端口或 LPR
TCP `515`。SNMP 负责提供目录，打印数据仍经所选 RAW/LPR 监听器传输。

客户端添加“标准 TCP/IP 端口”时，输入 PrintHub 的 IP 地址；如果出现多个端口
选择页，按名称选择目标打印机/端口，再核对摘要中的 RAW 端口号或 LPR 队列名。
具体向导行为须在目标 Windows 版本验收。PPM 不会一次性创建多台 Windows 打印机，
每个客户端打印机仍须分别选择自己的端口和驱动。

新建端口时，PPM 默认不启用 Windows 的「SNMP 状态已启用」，避免后台没有状态
数据时影响客户端发送作业。SNMP 发现仍可用；显式查询可使用 `public` 和对应
设备索引读取状态缓存。已有 Windows 端口不会自动修改：如需让旧端口也不依赖
状态轮询，请在客户端「打印机属性 → 端口 → 配置端口」取消「SNMP 状态已启用」。
如果仍需显式查询状态，增删或重命名映射后应重新核对设备索引。

## 目录内容与编号

目录包含 PPM General、Printer、Port 三组可读取对象；打印机和端口数量均与
当前启动配置一致。同一 Windows 打印队列的 RAW 与 LPR 映射合并为一台逻辑打印机。
每个 RAW 监听端口和每个 LPR 队列都是独立的端口记录，端口名称包含协议和端点。
本机 Windows 11 的 `GetPortList` 对每台逻辑打印机只返回首选端口；同一打印机
配置多个 RAW/LPR 通道时，MIB 保留完整表，但向导不会列出所有通道。

编号完全由配置确定，不写索引文件：

- 打印机名称按 Windows ordinal、不区分大小写的规则分组和排序，
  `ppmPrinterIndex` 从 `1` 开始分配，不依赖系统语言区域或打印机枚举顺序。
- 每台打印机内先按 RAW TCP 端口号升序排列，再按不区分 ASCII 大小写的 LPR
  队列名排列；`ppmPortIndex` 在该打印机内从 `1` 开始分配。
- 第一个端口为首选端口：优先最小 RAW 端口；没有 RAW 时选第一个 LPR 队列。

相同映射在重启后编号相同；调整配置文件中的映射排列顺序不会改变编号。
增删映射、重命名打印队列或改变端口可能重新分配编号。
目录在启动时生成，修改配置后须重启服务。

PPM 启用且 `printerMap` 非空时，`lpdServer.port` 必须为 TCP `515`；
队列名为 `1` 至 `32` 字节的可打印 ASCII（`0x21`–`0x7e`），不得包含空白、
控制字符或 `=`。对外公布的 LPR 字节计数为启用。设置 `"ppmDisabled": true`
后不再施加这些 PPM 专用限制；仍须满足配置文件的类型、范围、唯一性和编码规则。
无论是否启用 PPM，LPR 队列名都不得包含 CR/LF。

配置文件使用 UTF-8；Windows 后台打印机名称读入后保持配置模块与 `OpenPrinterA`
使用的 Windows ANSI 代码页，系统代码页无法表示的字符会报错。生成 MIB 时转换
为 Unicode，再以 UTF-8 导出。中文后台打印机名称
与 LPR 队列名是两个不同字段：后台名称可使用系统代码页支持的中文，LPR 队列名
仍必须是 ASCII。编码无法解码或名称包含内嵌 NUL 时启动失败。打印机显示名称同时
限制为 `127` 个 UTF-8 字节和 Windows TCPMON 能完整保存的 `63` 个 UTF-16 单元，
截断时不拆分字符。截断造成重名时，追加确定性的 ` [编号]` 后缀，保证向导能够区分。
端口名称仍限制为 `127` 个 UTF-8 字节，并保留协议/端点后缀。原后台映射保持不变。

## 后端状态与真实设备身份

PPM 启用时同时启动后台元数据采集，无需手填身份字段。每台逻辑打印机的
`ppmPrinterHrDeviceIndex` 等于其 `ppmPrinterIndex`，
`ppmPrinterSnmpQueryEnabled=false(2)`。显式查询 Host Resources MIB 仍可读取 `hrDeviceIndex`、
`hrDeviceType`、`hrDeviceStatus`、`hrPrinterStatus` 和 `hrPrinterDetectedErrorState`。
状态来自实际 Windows 后台打印队列，不宣告耗材、纸张、双面器等完整 Printer MIB
能力；`ppmPortPrtChannelIndex` 仍为 `0`。

每台队列的状态查询完成后至少间隔 `5` 秒再查。SNMP 请求只读取缓存，不同步
调用后台或驱动。未采集、查询失败、队列无法打开或样本超过 `30` 秒时返回未知
状态，不置故障位。已成功读取的脱机、缺纸、卡纸等状态按实际标志映射。后台返回
标志为 `0` 只表示队列没有报告故障，不能证明物理设备在线。

IEEE-1284 Device ID 优先通过 Windows `IBidiSpl` 的 `Get` 请求
`\Printer.DeviceInfo:IEEE1284DeviceID` 自动获取。没有有效非空身份时，再按下述门槛
尝试 USB 备用路径。只有实际设备/驱动返回且通过格式校验的身份才填入
`ppmPrinterIEEE1284DeviceId`；两条路径均未成功时保持空值。
成功身份缓存至服务重启；明确不支持的结果也缓存至重启，其他失败约 `60` 秒后
重试。驱动匹配仍需要客户端已安装与该真实身份兼容的驱动，不下载驱动，也不从
队列显示名称推造制造商/型号。

PrintHub 以单进程运行。状态和身份各用一个后台线程，服务不等待初次采集。查询
或清理一直不返回时，仅占用对应线程；不强杀线程，也不创建替代线程。队列读取
不提交打印任务、不修改现有队列。缓存不落盘，服务重启后重新采集。

停机时停止调度，先收尾 SNMP 和 RAW/LPD，再给两个采集线程共用的 `500` 毫秒
退出预算。采集仍卡住时，记录最后阶段并结束 PrintHub 自身，不终止系统打印后台。
这个期限不约束正在提交的 RAW/LPD 作业，也不保证内核 I/O 立即退出。由于采集和
打印在同一进程，驱动的进程内崩溃不再隔离。

### USB 身份备用路径与诊断日志

USB 备用路径随 PPM 启用，不需要单独配置，不改变 RAW/LPD 模式。
它只在 Bidi 未取得合法非空 ID 后尝试，并满足以下条件：

- 查询的是本机队列、单个端口；不支持远程队列或多端口打印池，不猜测设备。
- `GetPrinterW(level=2)` 未报告 `OFFLINE/NOT_AVAILABLE/SERVER_UNKNOWN/PENDING_DELETION`，
  且未设置“脱机使用打印机”（`PRINTER_ATTRIBUTE_WORK_OFFLINE`）。暂停不等于脱机。
- `EnumPortsW/EnumMonitorsW` 核对实际监视器为系统 `usbmon.dll`；仅端口名为 `USB001`
  不构成证据，“启用双向支持”复选框也不替代监视器判断。
- 通过 SetupAPI 枚举当前存在的 `GUID_DEVINTERFACE_USBPRINT`，使用设备配置中的
  `PortName` 或 USBMON 接口配置中的 `Port Number` 精确对应队列端口。必须唯一匹配；
  缺失、冲突或多台匹配均跳过，不按设备枚举顺序、型号、HardwareID 或 USB 插槽号猜测。

这些端口映射值属于 Windows USBMON 的实现配置，不是所有厂商都会提供的统一
设备属性。本轮没有 USB 真机；映射读取与具体驱动返回格式仍需实机确认。日志会
显示映射来源、候选编号、逻辑端口值及错误，便于补验；没有映射时安全失败。

唯一匹配后，以只读访问权限打开 SetupAPI 返回的设备接口，调用
[`IOCTL_USBPRINT_GET_1284_ID`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/usbprint/ni-usbprint-ioctl_usbprint_get_1284_id)。
不发送打印数据、不创建作业、不写设备配置，也不改队列或注册表。固定使用
`4094` 字节缓冲区；剥离长度前缀和传输终止符后，原样校验 ASCII、MFG/MDL、
`1023` 字节限制。超长、畸形或截断数据不公布。读取前后再次核对队列端口和
未脱机条件，变化时丢弃结果，避免状态串台。

USB 脱机、找不到设备、共享/权限/读取错误等保留为可重试失败，约 `60` 秒后再试，
不会因第一次 Bidi 不支持而永久跳过 USB。成功身份仍缓存至重启；已确定非 USB
且 Bidi 明确不支持的结果继续负缓存至重启。

Bidi 与 USB 备用路径共用身份线程。如果 Bidi 或其清理不返回，暂时无法执行 USB
或采集其他队列的身份；状态采集仍可运行。SNMP 始终只读缓存。

状态和身份诊断默认写入 **debug** 日志，默认的 `"info"` 级别不会输出。需要采集身份诊断时，
将 `logLevel` 设为 `"debug"`，并将 `name` 替换为实际 USB 打印队列名称：

```json
{
  "logLevel": "debug",
  "printerMap": [
    {
      "name": "实际 USB 打印队列",
      "lprQueue": "usb",
      "rawServer": {
        "listenAddress": "0.0.0.0",
        "port": 9100
      }
    }
  ],
  "ppmDisabled": false
}
```

首次部署时，在管理员 PowerShell 中安装并启动：

```powershell
.\PrintHub.exe --config .\config.json --service install
.\PrintHub.exe --service start
```

若已安装并使用该配置文件，保存修改后执行 `.\PrintHub.exe --service restart`。

日志位于可执行文件旁的 `logs` 目录。按 `Printer identity index` 查找：

- `bidi/`：COM、绑定、SendRecv、请求层结果、返回类型及长度；错误同时含十进制与十六进制。
- `usb/gate` / `usb/skip`：原始队列、端口、监视器 DLL、状态/属性及跳过原因。
- `usb/mapping` / `usb/candidate` / `usb/match`：注册表读取错误、端口证据和唯一匹配结果。
- `usb/open` / `usb/ioctl`：设备打开、调用开始、返回字节数及失败码。
- `identity/source`：成功来源（Bidi/USB）、合法 ID 长度及缓存策略。

采集线程不直接写日志。每个线程在内存中保留最近 `64` 条、每条最多 `511` 字节
的诊断，由服务线程定期读取；状态和身份诊断都写入 debug 日志，停机超期会记录最后阶段。
普通日志不记录完整 IEEE-1284 ID、USB 序列号路径、community 或报文。

## 协议与兼容性

SNMP 支持 v1/v2c 的 Get、GetNext，以及 v2c GetBulk；community 固定为 `public`。
请求和响应上限为 `4096` 字节；畸形或超限请求、未知 community 或不支持的 PDU
静默丢弃。SNMP Set 不支持。

项目以 Windows 7 为编译目标；原生端口发现、选择设备和参数填充在 Windows 11 上
验证过，Windows 7 的向导和运行兼容性仍待验证。真实 USB 设备身份获取、非空
Device ID 自动匹配驱动尚未完成实机验证。
验证 USB 路径时，可使用前文的 debug 日志查看获取阶段与错误码。
