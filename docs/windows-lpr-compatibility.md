# Windows LPR compatibility validation

Run the service with an LPD listener (default TCP 515) and expose a shared or
local Windows printer whose share name is used as the LPR queue name.

## Required matrix

| Windows client configuration | Expected server behavior |
| --- | --- |
| Standard TCP/IP Port, RAW, TCP 9100 | Streams every byte through to the configured RAW printer after TCP EOF. |
| Standard TCP/IP Port, LPR, byte counting enabled | Accepts exact file counts, a trailing NUL, and sends the second ACK. |
| Standard TCP/IP Port, LPR, byte counting disabled | Accepts a zero or placeholder data-file count and commits the received bytes on TCP EOF. |
| Optional feature **LPR Port Monitor** | Accepts RFC 1179 source ports 721-731 and the normal counted LPR exchange. |

## Manual procedure

1. Create a Windows test printer and either share it or use its exact local
   printer name as the configured LPR queue.
2. Configure a Standard TCP/IP port to the PrintHub host. Test RAW/9100 and
   LPR/515 separately; for LPR test with **LPR Byte Counting Enabled** both
   selected and cleared.
3. Submit a text page, a binary driver job, and a job larger than one TCP
   receive buffer. Verify the Windows spooler records the control-file `J`
   value as the document name and that byte content is not truncated.
4. Install the Windows optional feature **LPR Port Monitor**, create an LPR
   port, and repeat the counted LPR test. The source port may be 721-731;
   PrintHub deliberately does not reject it.
5. While a client is connected, stop PrintHub. Confirm the service enters
   `STOP_PENDING`, exits promptly, and leaves no uncommitted printer job.

The byte-count-disabled case is expected to log a `byte-count compatibility
fallback` warning when Windows closes the connection before its advertised
placeholder count. That event is successful compatibility behavior, not a
failed print job.
