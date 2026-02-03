# scp_rdma - SCP over RDMA

基于 RDMA 的高性能文件传输工具。

## 快速开始

### 编译
```bash
cd scp_rdma
meson setup build
ninja -C build
```

### 安装到系统 (可从任意路径使用)
```bash
chmod +x install.sh
./install.sh
```

### 卸载
```bash
./install.sh uninstall
```

### 自定义安装路径
```bash
./install.sh --prefix=$HOME/.local
```

## 使用方法

**服务端 (接收文件):**
```bash
scp_rdma -s
```

**客户端 (发送文件):**
```bash
scp_rdma <本地文件> <服务器IP>:<远程路径>

# 示例
scp_rdma myfile.txt 192.168.1.100:/tmp/
scp_rdma -r mydir 192.168.1.100:/data/  # 递归传输目录
```

## 选项

| 选项 | 说明 |
|------|------|
| `-s` | 服务端模式 |
| `-r` | 递归传输目录 |
| `-d <dev>` | 指定 RDMA 设备名 |
| `-p <port>` | TCP 端口 (默认 1777) |
| `-o <dir>` | 接收文件输出目录 |
| `-v` | 详细输出 |
| `-h` | 显示帮助 |
