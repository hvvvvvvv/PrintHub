# Windows SNMP discovery fast rejection

Use `--snmp-reject` when Windows' **Add Standard TCP/IP Printer Port Wizard**
waits for SNMP discovery requests to time out before it displays the generic
network-card option. PrintHub then listens on the configured listen address at
UDP port 161 and immediately returns a standards-compliant negative response
to SNMP v1/v2c `public` community Get and GetNext requests.

Example:

```powershell
PrintHub.exe --raw-server "Local Printer:9100" --snmp-reject
```

Open inbound UDP port 161 in Windows Firewall. If another service already owns
UDP 161, PrintHub fails to start rather than silently disabling this feature.

This feature does not emulate a printer and does not make automatic printer
identification succeed. In the wizard, select **Generic Network Card**, use
RAW protocol on TCP port 9100, and clear **SNMP Status Enabled** after the port
is created. RAW printing still requires a configured `--raw-server`; LPR
printing still uses the configured LPD listener.
