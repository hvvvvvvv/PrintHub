# PrintHub Web 面板、热重载与 HTTP API

## 概述

Web 面板内置于 `PrintHub.exe`，无需另外部署页面文件。启用后的默认入口是
<http://127.0.0.1:8323/>，仅供本机访问；局域网访问需要显式绑定服务器 IPv4 地址或
`0.0.0.0`，并放通对应 TCP 端口。页面可查看运行状态、最近日志和编辑打印配置。

所有 `/api/*` 请求均需 token 鉴权。服务只提供 HTTP，没有内置 TLS；token 会以明文
传输，仅应在受信任网络使用，局域网部署建议使用 VPN 或提供 TLS 的反向代理。

## 默认参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--web-bind <IPv4>` | `127.0.0.1` | 仅本机回环地址；`0.0.0.0` 表示所有 IPv4 接口。 |
| `--web-port <1-65535>` | `8323` | HTTP TCP 端口，禁止使用 `0`。 |
| `--web-token <token>` | 每次启动随机生成并打印到日志 | 显式值须为 16–256 个可打印 ASCII 字符（`0x21`–`0x7e`，不含空格），原样使用。 |

三个参数均支持空格与等号形式，例如 `--web-port 8323` 与 `--web-port=8323` 等价。
缺少值、非法地址/端口/token 或重复指定同一参数都会报错。`--help` 列出默认值与局域网示例。
这些参数来自启动命令行或服务 `ImagePath`，不属于 `config.json` 字段；
省略 `--web-token` 时由程序在每次启动时生成 token。

未传任何 Web 参数时，程序使用随机 token 在默认地址启用面板。随机数生成或 Web
绑定失败时，面板被禁用并记录 error，打印服务继续运行；非法 CLI 值会在启动时直接报错。

## 启用步骤

以下首次启用示例假定发布包已解压到 `C:\PrintHub`，并已从 `config.example.json`
准备好 `config.json`。在该目录打开管理员 PowerShell。

### 1. 选择显式 token 或启动时随机生成

需要固定 token 时，使用 `--web-token "ChangeMe-0123456789"`，将示例替换为自己的值。
token 长度必须为 `16`–`256`，每个字符必须位于 `0x21`–`0x7e`，不能含空格或其他
空白。字符串原样使用，不会去除首尾字符；请按所用 shell 的规则引用特殊字符。

省略 `--web-token` 时，程序通过 Windows `SystemFunction036`（RtlGenRandom）生成
32 字节密码学随机数，编码为 64 个小写十六进制字符。每次进程或服务启动都会重新生成，
重启后浏览器需输入新值；需要跨重启保持 token 时应显式指定。

两种方式都会在每次启动时将当前 token 写入 `logs\PrintHub_YYYY-MM-DD.log`，
即使配置 `logLevel` 高于 `info` 或为 `off`。随机模式的日志行是
`Web UI access token generated for this start: <token>`，显式模式为
`Web UI access token: <token>`。该行在尝试启动 Web 之前写入，
即使 Web 端口被占用也可查到；打印配置热重载不会更换或重复打印 token。

启动后可在程序目录的另一个 PowerShell 窗口查找最近一条 token 日志：

```powershell
Select-String -Path .\logs\PrintHub_*.log -Pattern 'Web UI access token' | Select-Object -Last 1
```

### 2. 选择前台运行或服务运行

仅本机访问，前台启动并随机生成 token：

```powershell
.\PrintHub.exe -c .\config.json
```

局域网访问，前台启动并指定固定 token：

```powershell
.\PrintHub.exe -c .\config.json --web-bind 0.0.0.0 --web-port 8323 --web-token "ChangeMe-0123456789"
```

也可以将上述局域网参数保存为 Windows 服务的启动参数：

```powershell
.\PrintHub.exe -c C:\PrintHub\config.json --web-bind 0.0.0.0 --web-port 8323 --web-token "ChangeMe-0123456789" --service install
.\PrintHub.exe --service start
```

安装时把配置文件的绝对路径与显式 Web 参数写入服务 `ImagePath`，显式 token 的原始值
也会写入，在注册表中可见，应限制服务配置的访问权限。未指定 token 时不会把 token
写入 `ImagePath`，服务每次启动时随机生成；例如仅本机访问的随机模式安装命令：

```powershell
.\PrintHub.exe -c C:\PrintHub\config.json --service install
.\PrintHub.exe --service start
```

前台与服务运行择一使用，避免端口冲突。已有服务要更换 Web 启动参数时，先停止并卸载，
再按新参数安装、启动；仅执行 `--service start` 不会改写已安装的 `ImagePath`。
随机模式在每次重启后需从本次启动日志取得新 token，并在浏览器重新输入。

### 3. 按需开放局域网访问

仅本机使用无需为远程访问新增入站规则。局域网示例允许同一子网访问专用网络配置文件下的
TCP `8323`；端口、网络配置文件和来源地址应按实际部署调整：

```powershell
New-NetFirewallRule -DisplayName "PrintHub Web" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8323 -Profile Private -RemoteAddress LocalSubnet
```

Windows 7 没有 `New-NetFirewallRule`，需使用等价的 `netsh` 命令：

```powershell
netsh advfirewall firewall add rule name="PrintHub Web" dir=in action=allow protocol=TCP localport=8323 profile=private remoteip=localsubnet
```

客户端访问 `http://<服务器IPv4>:8323/`；`0.0.0.0` 是监听地址，不是浏览器访问地址。
此规则仅涉及 Web 端口，打印所需的 RAW/LPD TCP 端口与 SNMP UDP `161` 仍需按部署放通。

## 使用面板

1. 在浏览器打开本机地址 <http://127.0.0.1:8323/> 或服务器的局域网地址。
2. 首次访问时，在令牌对话框输入本次服务启动日志中的 token（或 `--web-token` 指定的值）。页面将其保存在当前标签页的
   `sessionStorage`，用于后续 API 请求。
3. 查看运行状态卡，包括版本、配置路径、服务/前台模式、运行时长、RAW/LPD 端点与
   PPM 状态；最近日志区域从服务端定期刷新。状态和日志每 3 秒轮询一次，页面隐藏时暂停。
4. 编辑打印机映射和其他打印配置，也可导入 JSON、预览、复制或下载配置。
   Windows 打印机名称为可编辑下拉框，列出服务端枚举的本机已安装队列，不含远程连接；
   也可手工输入实际 Windows 队列名。LPR 队列名必须为单行，输入框保留自动高度，
   不可手动拖拽调整大小。
5. 点击「保存配置」，确认会中断活动打印连接后提交。导入 JSON 仅填充表单，保存后才生效。

打印机列表在配置加载成功后获取，之后聚焦名称输入框时按 60 秒成功缓存刷新。
列表读取失败会静默保留旧列表（首次失败则为空），仍可手工输入和保存；`401` 沿用令牌重输流程。
下拉建议不会限制输入值，名称仍须满足 [ACP 编码限制](configuration.md#编码限制) 和服务端校验。

也支持 `http://127.0.0.1:8323/?token=<URL编码后的token>` 一次性入口：页面优先读取
查询参数，再将其从地址栏移除，并保存到 `sessionStorage`。这是一次性传入 token 的方式，
不会让 token 本身一次使用后失效。显式 token 中的 `+`、`/`、`=`、`&` 等字符应进行 URL 编码；
含 token 的链接仍可能进入访问日志或被转发，优先使用令牌输入框。

## 保存与热重载

保存通过 `PUT /api/config` 提交完整配置，遵循与文件加载相同的校验规则：

1. 校验 JSON；与当前运行配置相同且运行时正常时，不重建监听器，但仍检查磁盘文件，
   文件缺失、无法读取或内容不一致时会尝试写回修复。
2. 有变化时，先原子写回当前配置路径对应的同一文件（默认 `config.json`，也可以是
   `-c/--config` 指定的文件）。
3. 在线停止旧 RAW/LPD 监听器和 PPM，再按新配置重建；`logLevel` 同时即时生效。

**不会重启 Windows 服务，Web 面板保持可用。活动打印连接会中断**，所以面板在提交前
请求确认，应选择合适的时间保存。应用失败会自动尝试恢复旧运行配置并回写旧文件；
回滚本身也可能失败，需结合响应的 `rolledBack`、`apply.lastError` 和日志确认结果。

外部直接修改 `config.json` 不会触发热加载。服务模式仍需执行
`.\PrintHub.exe --service restart`，前台模式需重新启动进程；也可将修改后的文件导入面板
再保存。面板读取当前运行配置，导入前应避免直接保存旧表单覆盖外部修改。
Web 监听地址、端口和 token 不属于打印配置，不能通过这次热重载改变。

## HTTP API 参考

API 认证头为 `X-PrintHub-Token`，所有 `/api/*`（包括不存在的 API 路径）均需提供。
缺少或错误的 token 返回 `401` 和 `{"error":"unauthorized"}`，不区分具体鉴权失败原因，
也不回显 token。静态 HTML 无需认证，响应类型为 `text/html; charset=utf-8`。

| 方法 | 路径 | 说明 | 状态码 |
| --- | --- | --- | --- |
| `GET` | `/`、`/index.html` | 内嵌页面。 | `200` |
| `GET` | `/api/status` | 运行状态 JSON；状态读取异常时返回错误。 | `200`、`401`、`500` |
| `GET` | `/api/config` | 完整的当前运行配置 JSON；读取异常时返回错误。 | `200`、`401`、`500` |
| `PUT` | `/api/config` | body 为完整配置 JSON，保存并热应用；必须有合法的 `Content-Length`。 | `200`、`400`、`401`、`409`、`411`、`413`、`415`、`500` |
| `GET` | `/api/logs?tail=N` | 最新日志文件末尾至多 N 行，返回 `{"lines":[...]}`；文件读取失败返回错误。 | `200`、`401`、`500` |
| `GET` | `/api/printers` | 需要 token；返回本机已安装打印机队列名称的 UTF-8 字符串数组，排除远程连接；枚举失败返回错误。 | `200`、`401`、`500` |
| 其他 | `/api/*` | 已鉴权但没有匹配接口时返回 `{"error":"not found"}`；正文方法会先被拒绝。 | `404`、`401`、`405`、`411` |

日志 `tail` 范围为 `1`–`500`，省略或解析失败时默认 `100`，可解析的越界值会限制到该范围；
无日志返回 `{"lines":[]}`。日志来源是可执行文件目录中 `logs` 下最新的 `PrintHub_*.log`，
仅读取该文件末尾 64 KiB，并丢弃窗口开头不完整的一行，因此可能少于 N 行；单行超过
读取窗口且窗口内没有完整行时，可能返回空数组。文件打开、定位或读取失败时返回 `500`。
API 将日志正文按 UTF-8 输出，无效字节以替换字符显示；现有部分日志消息含系统 ANSI
代码页（ACP）的打印机名称，在非 UTF-8 系统代码页下可能显示替换字符，不影响打印功能。
日志读取失败只在日志区域提示，不会把状态正常的服务标记为离线或阻止保存。

`GET /api/printers` 每次请求使用 `EnumPrintersW(PRINTER_ENUM_LOCAL, level 2)` 重新枚举，
过滤带 `PRINTER_ATTRIBUTE_NETWORK` 的远程连接，保留本地 TCP/IP 端口上的队列。
名称按大小写不敏感规则排序并去重；无队列返回 `{"printers":[]}`，枚举失败返回 `500`
及 JSON `error`。返回的是 PrintHub 服务端进程可枚举的队列，不是浏览器所在电脑的打印机。
例如：

```json
{"printers":["Microsoft Print to PDF","OneNote (Desktop)"]}
```

API 响应带 `Cache-Control: no-store`，响应带 `X-Content-Type-Options: nosniff`，
服务不发送 CORS 头。静态页还带 `X-Frame-Options: DENY` 和含
`frame-ancestors 'none'` 的 `Content-Security-Policy` 响应头，禁止被嵌入框架。

正文读取前先完成 API token 校验；`GET`/`HEAD` 携带正文长度或传输编码头时返回 `400`，
其余请求携带 `Transfer-Encoding`（包括 chunked）时返回 `411`。
`POST`/`PUT`/`PATCH`/`DELETE` 只允许 `PUT /api/config`，其余返回 `405`；
其他方法若携带 `Content-Length` 也返回 `405`，无正文头则按原路由处理。
`PUT /api/config` 缺少、重复或无法解析 `Content-Length` 时返回 `411`，超过 1 MiB 时返回 `413`。

### 查询状态与提交配置

以下 PowerShell 示例使用 `curl.exe`，将 `<token>` 替换为实际值；PUT 会中断活动打印连接，
调用 API 时没有页面确认步骤。`curl.exe` 需在系统中可用，Windows 7 需另行准备该工具。

```powershell
curl.exe -H "X-PrintHub-Token: <token>" http://127.0.0.1:8323/api/status
curl.exe -X PUT -H "X-PrintHub-Token: <token>" -H "Content-Type: application/json" --data-binary "@config.json" http://127.0.0.1:8323/api/config
curl.exe -H "X-PrintHub-Token: <token>" "http://127.0.0.1:8323/api/logs?tail=100"
curl.exe -H "X-PrintHub-Token: <token>" http://127.0.0.1:8323/api/printers
```

`GET /api/status` 示例（局域网监听配置，示例值不表示默认值）：

```json
{
  "version": "1.0.2",
  "configPath": "C:\\PrintHub\\config.json",
  "mode": "service",
  "startedAt": "2026-11-11T19:00:00Z",
  "uptimeSeconds": 1234,
  "service": { "installed": true, "running": true },
  "web": { "listenAddress": "0.0.0.0", "port": 8323 },
  "lpd": { "listenAddress": "0.0.0.0", "port": 515, "running": true },
  "rawServers": [ { "printerName": "HP LaserJet Pro M404", "listenAddress": "0.0.0.0", "port": 9100 } ],
  "mappingCount": 1,
  "ppm": { "enabled": true, "running": true },
  "apply": { "state": "idle", "lastError": null, "lastAppliedAt": "2026-11-11T19:05:00Z" }
}
```

`mode` 为 `service` 或 `console`；服务状态查询失败时，`service` 为 `null`。
`apply.state` 为 `idle`、`applying` 或 `error`，`lastError` 为错误说明或 `null`，
`lastAppliedAt` 为最后应用时间或 `null`。时间采用 UTC ISO 8601，`uptimeSeconds` 单位为秒。
热应用期间状态读取不等待配置重建，`apply.state` 可为 `applying`，其余配置与监听字段
可能来自应用前的缓存快照；应用结束后刷新为当前状态。

### 保存响应与错误

| 状态码 | 含义与处理 |
| --- | --- |
| `200` | 成功应用返回 `{"applied":true,"unchanged":false}`；与当前运行配置相同返回 `{"applied":false,"unchanged":true}`，不重建监听器；若同时修复了不一致或无法读取的配置文件，响应还包含 `"fileRepaired":true`。 |
| `400` | 配置 JSON 解析或校验失败，或 `GET`/`HEAD` 带正文相关头；查看 `error` 并修正后重试。 |
| `401` | token 缺少或错误，重新输入有效 token。 |
| `405` | 正文方法或路径不受支持；仅允许 `PUT /api/config` 提交正文，在读取正文前拒绝。 |
| `409` | 另一项配置正在应用，返回 `{"error":"configuration apply already in progress"}`；等待完成后重试。 |
| `411` | 除 `GET`/`HEAD` 外的请求含 `Transfer-Encoding`，或配置 PUT 缺少、重复、无法解析 `Content-Length`；在读取正文前拒绝。 |
| `413` | 请求体的 `Content-Length` 超过 `kMaxConfigurationFileBytes`，即 `1 MiB`；在读取正文前拒绝。 |
| `415` | PUT 的媒体类型不是 `application/json`，设置正确的 `Content-Type`。 |
| `500` | 保存文件或运行时应用失败，查看 `error`、回滚结果与日志。 |

应用失败的 `500` 响应可包含 `{"error":"...","rolledBack":true}` 或
`{"error":"...","rolledBack":false}`。`rolledBack: true` 表示已回滚到旧配置；
`false` 表示回滚未完成，不能假定打印服务仍正常。文件保存阶段就失败时不一定存在该字段，
字段缺失表示未确认回滚结果。检查 `GET /api/status` 的 `apply`、打印监听状态和日志。
回滚未完成可能是运行时已恢复、但旧文件回写失败，此时磁盘与运行配置不一致；再次保存
相同的当前运行配置会校验并修复文件，成功返回 `fileRepaired:true`，修复写盘失败仍返回 `500`。
若运行时恢复失败、监听器已停止，修正配置后可直接在面板再次保存，成功后监听器会
重新启动，无需重启服务；若面板也无法使用，再恢复可用配置文件并重启服务。
回滚成功后 `apply.state` 仍可能为 `error`，用于保留失败原因。

## 安全说明

- 所有 `/api/*` 都需要同一 token；比较采用常量时间方式。没有多用户或角色权限区分，
  持有 token 就可以读取状态、配置、日志并修改打印配置。
- 静态页面公开，但只包含界面资源，不内嵌 token、运行配置或日志；这些数据均由鉴权 API 提供。
- HTTP 会明文传输 token 和 API 数据；仅在受信任网络使用，并用防火墙限制来源地址。
  局域网部署建议使用 VPN 或提供 TLS 的反向代理，反向代理后端仍应限制为可信访问范围。
- 服务端限制为 4 个 HTTP worker、最多 16 个排队连接，读等待超时为 10 秒、写等待超时
  为 30 秒，并在读取正文前限制方法、长度和传输编码；这些限制并不保证整个请求在
  10 秒内结束。生产进程退出时不等待仍在收发的 HTTP 请求；这些有界处理不能替代可信网络隔离。
- 每次启动都会把当前 token 写入日志；显式 token 还会出现在命令行与服务 `ImagePath`
  中。限制日志目录与服务配置的访问权限，避免泄露含 token 的日志、URL、命令行和截图。
- 轮换显式 token 时，更新启动参数并重启；服务模式需更新安装时保存的参数。
  随机模式重启即可更换 token。两种模式都需要在浏览器输入新值，保存打印配置不会轮换 token。

## 故障排查

| 现象 | 检查与处理 |
| --- | --- |
| 面板打不开 | 查看 `<exe目录>\logs\PrintHub_YYYY-MM-DD.log` 中的 `Web UI disabled: ...`；检查随机数生成是否失败、端口是否被占用、绑定地址是否有效且属于本机。非法 CLI 值会在启动时直接报错。 |
| 本机可用，局域网无法访问 | 默认只绑定 `127.0.0.1`；显式指定服务器 IPv4 或 `0.0.0.0`，确认服务 `ImagePath` 中的参数与入站防火墙规则，使用服务器实际 IPv4 访问。 |
| 返回 `401` | 检查请求头 `X-PrintHub-Token` 是否与本次启动日志中的 token 一致；随机模式每次重启都会变化，页面会提示重新输入。 |
| `--web-token` 参数报错 | 检查是否缺值、重复指定，或值不满足 16–256 个可打印 ASCII 字符且不含空格；按 shell 规则引用特殊字符。 |
| 返回 `409` | 等待另一项配置应用结束，再刷新状态并重试。 |
| `apply` 失败或返回 `500` | 查看 `error`、`rolledBack`、`apply.lastError` 与日志；检查配置文件写入权限、打印端口冲突等。回滚未完成时恢复有效配置并按需重启。 |
| 保存时打印连接断开 | 热重载会在线重建打印监听器，这是预期行为；安排无活动打印连接时保存，确认客户端任务状态后再提交打印。 |

## 限制

- 面板不提供 Windows 服务的启动、停止、重启或卸载操作；请使用 `--service` 命令。
- 打印机下拉仅列出服务端枚举的本机队列，不含远程连接；未列出的名称可手工输入，
  保存时仍以服务端校验和应用结果为准。下拉展示和键盘交互取决于浏览器的 `datalist` 支持。
- 没有内置 HTTPS，也没有用户、角色或不同 token 对应的权限分级。
- 不监视配置文件变化；Web 启动参数与 token 轮换不随打印配置热重载。
- Windows 7 SP1 x86 仍需实机验证，包括网络访问、服务运行和实际打印；不能以文档中的
  命令示例代替平台验证。
