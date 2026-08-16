# Windows LPR 兼容性验证

运行启用 LPD 监听器（默认 TCP 515）的服务，并显式配置
`--lpd-queue "队列名=打印机名称"` 映射。PrintHub 仅接受已配置的队列名；
不会根据 Windows 打印机或共享名称推断目标打印机。

## 必测矩阵

| Windows 客户端配置 | 预期服务器行为 |
| --- | --- |
| 标准 TCP/IP 端口、RAW、TCP 9100 | TCP 连接关闭后，将收到的全部字节流转发给配置的 RAW 打印机。 |
| 标准 TCP/IP 端口、LPR、启用字节计数 | 接受精确的文件长度、末尾 NUL，并发送第二个 ACK。 |
| 标准 TCP/IP 端口、LPR、禁用字节计数 | 接受零值或占位的数据文件长度，并在 TCP 连接关闭后提交已接收的字节。 |
| Windows 可选功能 **LPR 端口监视器（LPR Port Monitor）** | 接受 RFC 1179 规定的 721–731 源端口，以及正常的带计数 LPR 交互。 |

## 手动验证步骤

1. 创建 Windows 测试打印机，并在 PrintHub 管理面板中为其添加 LPD 队列映射。
   在 Windows LPR 端口中使用配置的队列名；队列名必须匹配，但 ASCII 字母不区分大小写。
2. 为 PrintHub 主机配置标准 TCP/IP 端口，分别测试 RAW/9100 与 LPR/515。
   LPR 测试应分别在选中和清除“启用 LPR 字节计数（LPR Byte Counting Enabled）”时执行。
3. 提交一页文本、一个二进制驱动程序任务，以及一个大于单次 TCP 接收缓冲区的任务。
   确认 Windows 打印后台处理程序将控制文件中的 `J` 值记录为文档名称，且字节内容没有截断。
4. 安装 Windows 可选功能 **LPR 端口监视器（LPR Port Monitor）**，创建 LPR 端口并重复带计数的 LPR 测试。
   源端口可以是 721–731；PrintHub 会有意接受这些端口。
5. 在客户端保持连接时停止 PrintHub。确认服务进入 `STOP_PENDING`、能及时退出，且没有未提交的打印任务。

禁用字节计数时，如果 Windows 在其声明的占位长度到达前关闭连接，预期日志会出现
`byte-count compatibility fallback` 警告。这表示兼容性回退成功，而非打印任务失败。
