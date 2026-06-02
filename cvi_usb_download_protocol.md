# CVI / CVITEK USB 下载协议整理

本文整理的是 `usb_dl` 在 CV180x/CV181x、Linux/XML 烧录路径中实际使用的下载协议。

## 1. 分层模型

协议可以分成三层：

```text
Linux 用户态工具
  |
  |  CVI 下载协议：cmd/header/CRC/ACK/stage/chunk
  v
传输层
  |  原版：libusb bulk OUT 0x01 / bulk IN 0x81
  |  当前实现：/dev/ttyACM* 字节流，Linux cdc_acm 负责 USB bulk endpoint
  v
目标 SoC ROM / 二阶段 downloader / U-Boot utask
```

关键点：ACM 只替代 USB endpoint 访问方式，不替代 CVI 下载协议。也就是说，即使用 `/dev/ttyACM*`，仍然需要自己构造 CVI message header、CRC、ACK 处理和烧录状态机。

## 2. USB / ACM 传输层

目标设备枚举为：

```text
VID:PID = 3346:1000
Product = USB Com Port
Manufacturer = CVITEK
Class    = CDC ACM
```

原版 libusb 路径使用：

```text
bulk OUT endpoint = 0x01
bulk IN  endpoint = 0x81
interface 0 + interface 1
CDC line coding = 921600 8N1
CDC control line state = 0
```

当前 ACM-only 实现使用：

```text
/sys/class/tty/ttyACM* 扫描设备
/dev/ttyACM* raw mode
921600 8N1
CLOCAL | CREAD
清 HUPCL
不拉高 DTR/RTS
```

设备在下载过程中会反复断开重连。常见阶段表现：

```text
初始 ROM 阶段 serial: 5340
发 magic 后可能重枚举
二阶段 serial: 123456789ABC
```

所以程序不能固定 `/dev/ttyACM0`，应按 VID/PID 和可选物理端口反复扫描。

## 3. 通用消息头

### 3.1 短消息头，8 字节

```text
offset  size  meaning
0x00    1     command
0x01    2     length，big-endian
0x03    5     address，40-bit big-endian
```

布局：

```c
h[0] = cmd;
h[1] = len >> 8;
h[2] = len & 0xff;
h[3] = addr >> 32;
h[4] = addr >> 24;
h[5] = addr >> 16;
h[6] = addr >> 8;
h[7] = addr;
```

注意：`length` 的语义依命令不同。

对于 `CVI_USB_TX_DATA_TO_RAM` / `CV_USB_KEEP_DL` 这类按文件切片发送的包，`length` 是本次 USB/CVI 包总长度，包含 8 字节 header。

例如发送一包 248 字节 FIP 数据：

```text
cmd      = 0x00
length   = 0x0100
address  = 0x0000000000
payload  = 248 bytes
总长度   = 256 bytes
```

首 8 字节：

```text
00 01 00 00 00 00 00 00
```

对于 `TX_FLAG`、`BREAK`、`UBREAK`、`PRG_CMD`、`REBOOT` 这类请求包，`length` 通常是 payload 长度；无 payload 时为 0。

### 3.2 长消息头，16 字节

长消息头用于 `CVI_USB_S2D`，即 host 向 device 发送大块数据。

前 8 字节同短消息头，后 8 字节是 `data_size`，little-endian：

```text
offset  size  meaning
0x00    1     command = 0x81
0x01    2     length = 0x0010，即 header 长度
0x03    5     address，40-bit big-endian
0x08    8     data_size，64-bit little-endian
```

流程是：

```text
host -> device: 16-byte S2D header
host <- device: 16-byte ACK，CRC 校验 header
host -> device: raw data bytes，长度 data_size
```

## 4. 命令号

CV180x/CV181x Linux 路径保留的命令号：

| 命令 | 值 | 方向 | 用途 |
|---|---:|---|---|
| `CVI_USB_TX_DATA_TO_RAM` | `0x00` | host -> device | 把小切片数据写入目标 RAM，ROM 阶段 FIP 和 U-Boot 阶段 FIP 使用 |
| `CVI_USB_TX_FLAG` | `0x01` | host -> device | 写 boot flag，例如 MGN1 |
| `CV_USB_BREAK` | `0x02` | host -> device | ROM 阶段 break / 跳转 / 切阶段 |
| `CV_USB_KEEP_DL` | `0x03` | host -> device | 发送 `cv_dl_magic.bin` |
| `CV_USB_UBREAK` | `0x04` | host -> device | U-Boot/utask 阶段发送 FIP 后 break |
| `CV_USB_PRG_CMD` | `0x06` | host -> device | 向 U-Boot/utask 发送命令字符串，例如 `setenv filesize ...` |
| `CVI_USB_REBOOT` | `0x16` | host -> device | 烧录完成后重启目标设备 |
| `CVI_USB_S2D` | `0x81` | host -> device | host 发送大块数据到 device RAM |
| `CVI_USB_D2S` | `0x82` | device -> host/request | host 请求从 device 读取数据，如 stage/update_addr |
| `CVI_USB_PROGRAM` | `0x83` | host -> device | 触发目标端把已发送 chunk 写入存储 |

## 5. CRC 与 ACK

### 5.1 CRC16

使用 CRC16-CCITT/XMODEM 风格：

```text
poly        = 0x1021
initial     = 0x0000
xorout      = 0x0000
refin/out   = false
```

C 形式：

```c
uint16_t crc = 0;
for each byte:
    crc ^= byte << 8;
    repeat 8:
        if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
        else              crc = crc << 1;
```

### 5.2 ACK 格式

工具通常读取 16 字节 ACK。

当需要 CRC 校验时：

```text
ack[2..3]   big-endian CRC16
ack[8..11]  big-endian fip_tx_offset
ack[12..15] big-endian fip_tx_size
```

`fip_tx_offset` 和 `fip_tx_size` 只在 ROM 二阶段补发 FIP 时有实际意义。

对以下命令，工具通常读 ACK 但不校验 CRC：

```text
TX_FLAG
BREAK
UBREAK
PROGRAM
REBOOT
PRG_CMD
```

其中 `BREAK`、`UBREAK`、`PROGRAM`、`REBOOT` 可能导致设备切阶段或长时间忙，读 ACK 超时/断开在某些实现里可以被视作可接受。

## 6. 固定 magic number / 地址

CV180x/CV181x Linux 路径关键常量：

```text
ROM VID/PID            = 3346:1000
DUMMY_ADDR             = 0xff
UBOOT_CMD_ADDR         = 0x04003000
MGN1 flag address      = 0x0e000004
MGN1 flag payload      = "1NGM"，字节 31 4e 47 4d
initial FIP offset     = 0
initial FIP size       = 0x1000
initial FIP addr       = 0
small packet size      = 0x100
small packet header    = 0x08
small packet payload   = 0xf8
S2D max transfer chunk = 0x80000
CIMG header size       = 0x40
CIMG raw chunk size    = 0x1000000
```

## 7. 文件切片发送格式

`send_file_sliced()` 用于发送 `cv_dl_magic.bin`、ROM 阶段 FIP、U-Boot 阶段 FIP。

每个小包最大 0x100 字节：

```text
8-byte CVI short header
最多 0xf8 bytes payload
```

每包发送后：

```text
expected_crc = CRC16(本次实际写出的整个包，包括 header + payload)
读取 16-byte ACK
比较 ack[2..3]
若校验成功，地址 += 实际 payload 长度
```

示例：发送 `cv_dl_magic.bin`，大小 128 字节：

```text
cmd      = 0x03
length   = 0x0088
addr     = 0x00000000ff
payload  = 128 bytes magic
总长度   = 136 bytes
```

示例：发送 FIP 首包：

```text
cmd      = 0x00
length   = 0x0100
addr     = 0x0000000000
payload  = 248 bytes fip.bin
总长度   = 256 bytes
```

第二包地址变成：

```text
addr = 0x00000000f8
```

## 8. ROM 阶段状态机，CV180x/CV181x

输入文件：

```text
cv_dl_magic.bin
fip.bin
```

### 8.1 第一轮 magic

```text
connect 3346:1000
send cv_dl_magic.bin:
    cmd  = CV_USB_KEEP_DL = 0x03
    addr = 0xff
restart / close / 等待设备重枚举
```

### 8.2 发送 FIP 初始 0x1000 字节

```text
connect 3346:1000
read fip.bin size
send fip.bin[0:0x1000]:
    cmd  = CVI_USB_TX_DATA_TO_RAM = 0x00
    addr = 0
send MGN1 flag:
    cmd     = CVI_USB_TX_FLAG = 0x01
    addr    = 0x0e000004
    payload = "1NGM"
send break:
    cmd  = CV_USB_BREAK = 0x02
    addr = 0xff
restart / close / 等待设备重枚举
```

### 8.3 ROM 二阶段循环

循环逻辑：

```text
connect 3346:1000
stage = get_dl_stage()
if stage 表示已离开 ROM downloader:
    退出 ROM 阶段，进入 U-Boot/utask 阶段
else:
    send cv_dl_magic.bin
    从 ACK 里取得 fip_tx_offset / fip_tx_size
    send fip.bin[fip_tx_offset : fip_tx_offset + fip_tx_size]
    send MGN1 flag
    send break
    restart / close / 等待设备重枚举
    继续循环
```

`get_dl_stage()` 的行为比较特殊：

```text
发送 D2S 请求：cmd=0x82, len=64, addr=0
读取 64 字节
只看 buf[6] 是否等于 0x82
```

原版行为是不严格检查读取返回值；如果读取失败，buffer 保持 0，`buf[6] != 0x82`，于是退出 ROM loop。这个行为在 ACM 后端很重要，因为阶段切换时 tty 可能表现为 EIO/timeout。

## 9. U-Boot / utask 阶段

ROM 阶段结束后，工具进入 U-Boot/utask 下载阶段。

### 9.1 获取 update address

```text
send D2S request:
    cmd  = CVI_USB_D2S = 0x82
    len  = 8
    addr = 0
read 8 bytes
update_addr = little-endian u64
```

这个地址后续用于 `S2D` 发送镜像数据。

### 9.2 解析 XML manifest

Linux/XML 路径查找固件目录里的 XML，一般是：

```text
partition_*.xml
```

解析：

```text
<storage type="emmc"> 或等价属性
<partition size_in_kb="..." file="...">
```

当前保留路径中：

```text
storage type == emmc 时，列表前置 fip.bin
partition 的 file/filename/image 属性加入下载列表
size_in_kb 为 0 或 file 为空/none/- 时跳过
```

Linux/XML 路径假设普通镜像已经是 vendor CIMG 包，不再把 raw 镜像包装成 CIMG。

### 9.3 U-Boot 阶段 fip.bin 特殊处理

如果 manifest 列表里遇到 `fip.bin`：

```text
send command:
    cmd     = CV_USB_PRG_CMD = 0x06
    addr    = 0
    payload = "setenv filesize 0x<当前 fip.bin 大小>" + NUL padding

send fip.bin:
    cmd  = CVI_USB_TX_DATA_TO_RAM = 0x00
    addr = update_addr

send ubreak:
    cmd  = CV_USB_UBREAK = 0x04
    addr = 0xff

restart / reconnect 3346:1000
```

`setenv filesize` 使用十六进制字符串，例如：

```text
setenv filesize 0x67c00
```

payload 长度按原版习惯是：

```text
strlen(command) + 8
```

也就是命令字符串后面多发几个 NUL padding。

## 10. CIMG 镜像发送与 PROGRAM

普通镜像文件按 CIMG 格式发送。

### 10.1 发送 CIMG header

先发送文件前 0x40 字节到 `update_addr`：

```text
send S2D data:
    cmd       = 0x81
    addr      = update_addr
    data_size = 0x40
    data      = 文件前 0x40 字节
```

然后本地重新读取这 0x40 字节，解析：

```text
offset  field
0x00    magic，一般是 "CIMG" 小端显示为 0x474d4943
0x08    chunk_header_size
0x0c    total_chunk
0x10    file_size / total payload size
```

日志中会打印：

```text
magic: %x, chunk_sz: %x, total_chunk: %x, file_size: %x
```

### 10.2 发送每个 chunk

循环 `total_chunk` 次，或直到 `remaining == 0`：

```text
tx_limit = chunk_header_size + 0x1000000
本次 tx = min(remaining, tx_limit)
通过 S2D 发送 tx bytes 到 update_addr
发送 CVI_USB_PROGRAM:
    cmd  = 0x83
    addr = 0x04003000
    no payload
等待目标写入完成
更新进度
```

S2D 自身会把大块数据再切成最大 0x80000 字节的写入块。

重要点：`CVI_USB_PROGRAM` 并不携带镜像数据；它的作用是告诉目标端“刚刚发到 RAM 的 chunk 可以写入存储了”。

## 11. reboot

所有 manifest entry 处理完成后：

```text
send reboot:
    cmd  = CVI_USB_REBOOT = 0x16
    addr = 0x04003000
    payload = empty
```

设备随后重启。

## 12. 成功路径日志对应关系

典型成功路径大概是：

```text
USB download start...
found acm device vid=0x3346 pid=0x1000 ... serial=5340
send magic bin
found acm device vid=0x3346 pid=0x1000 ...
fip.bin size: ...
set MGN1 flag
break
Connecting to ROM 2nd stage...
found acm device vid=0x3346 pid=0x1000 ...
xml file is .../partition_emmc.xml
xml storage type: emmc
update total size: ... byte
update address: 0x...
setenv cmd: setenv filesize 0x...
send fip.bin finish
break
downloading file: ...
magic: 474d4943, chunk_sz: ..., total_chunk: ..., file_size: ...
CVI_USB_PROGRAM
updated size: ...
USB download complete
reboot usb device
```
