# Windows SNMP 发现快速拒绝

当 Windows 的**添加标准 TCP/IP 打印机端口向导**在显示通用网络卡选项前等待
SNMP 发现请求超时时，可使用 `--snmp-reject`。启用后，PrintHub 会在配置的监听地址
上的 UDP 161 端口监听，并立即对 SNMP v1/v2c `public` 团体的 Get 与 GetNext 请求
返回符合标准的否定响应。

示例：

```powershell
PrintHub.exe --raw-server "本地打印机名称:9100" --snmp-reject
```

请在 Windows 防火墙中开放入站 UDP 161。若该端口已被其他服务占用，PrintHub 会启动失败，
而不会静默停用此功能。

此功能不会模拟打印机，也不能让自动识别打印机成功。在向导中请选择
**通用网络卡（Generic Network Card）**，使用 TCP 9100 上的 RAW 协议，并在创建端口后
清除**已启用 SNMP 状态（SNMP Status Enabled）**。RAW 打印仍须配置
`--raw-server`；LPD 打印仍使用已配置的 LPD 监听器。
