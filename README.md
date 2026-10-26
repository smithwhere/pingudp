# pingudp

Windows 图形界面 UDP 往返延迟探测器，配套 Linux VPS 回显服务。

## 下载和使用

从 [Releases](https://github.com/smithwhere/pingudp/releases) 下载 `udpping.exe`，在 Windows x64 上双击运行。输入 VPS 公网 IPv4 地址或域名，端口默认 `9000`，点击“开始”。“次数”设为 `0` 时持续探测，点击“停止”查看统计；间隔和超时单位都是毫秒。

在 Linux VPS 中以 root 执行下面这条完整命令，先允许所有来源访问 UDP 9000，再启动回显服务。需要已安装 Python 3、UFW 和 ss。

```bash
ufw allow 9000/udp && python3 - <<'PY'
import socket
import subprocess

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", 9000))
subprocess.run("ss -lun | grep ':9000'", shell=True, check=True)

while True:
    data, address = sock.recvfrom(64)
    if data.startswith(b"UPNG"):
        sock.sendto(data, address)
PY
```

执行期间保持 SSH 窗口打开。若端口已经被之前的回显程序占用，先在原窗口按 Ctrl+C 停止旧进程。UFW 处于 inactive 时，这条命令不会启用 UFW；Vultr 云防火墙也需要单独放行 UDP 9000。

服务端只回传 16 字节、以 `UPNG` 开头的数据报。客户端在包内加入会话标识和序号，检查来源与内容，使用高精度计时器计算往返时间。Windows 原生 `ping` 使用 ICMP，不会测到这个 UDP 服务的表现。

“未收到回复”比例是探测超时或错误比例，不是单向 UDP 丢包率。请求或回复可能在任一方向丢失，也可能被防火墙或服务端阻断；测得的 RTT 包含服务端处理时间。

## 源码和编译

- `client/udpping-gui.c`：双击运行的 Windows 图形界面客户端。
- `client/udpping-cli.c`：可选的命令行客户端，支持 `-n`、`-t`、`-i`、`-w` 参数。
- `server/udp_echo.py`：Linux VPS UDP 回显服务。

使用 MinGW-w64 x86_64 交叉编译器，在 Linux 中运行：

```bash
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -municode -mwindows -s -o udpping.exe client/udpping-gui.c -lws2_32
```

也可执行 `sh build-windows.sh` 同时编译图形界面和命令行版本。

Release 的 `udpping.exe` 为 Windows x64 图形界面程序。源码也包含命令行版本，可用相同编译器去掉 `-municode -mwindows` 并替换源码路径构建。

图形界面保留最近约 2000 行日志。新日志始终追加在末尾，点击日志中的旧内容不会改变新结果的显示位置。

## 验证

在 Linux 上编译相同的命令行探测逻辑，使用本地 UDP 回显服务测试了正常回复、一次故意不回复、超时统计和参数校验。Windows 图形界面程序已交叉编译并核对为 PE32+ GUI 可执行文件；尚未在实际 Windows 机器上做运行测试。
