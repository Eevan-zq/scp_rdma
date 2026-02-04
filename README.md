# scp_rdma - SCP over RDMA

## 工具：基于 RDMA 的高性能文件传输工具。
背景：在智算场景中，智算服务器之间通常由RDMA(RoCE、IB、iWARP)实现Scale Out通信，本项目的目的就是为了用RDMA协议栈完成跨服务器之间的文件/文件夹 的相互传递，在传输镜像以及模型权重等文件时能够更加迅速。

## 简介
结合实际的使用场景，本项目使用OOB(Socket)作为RDMA建链的方式，使用控制消息CDC(Connection Data Control)来通知接收端已完成write写入：
```bash
【问题】
发送端: RDMA Write 直接写入接收端内存
        ↓
接收端: 不知道数据什么时候到达！
【CDC 解决方案】
发送端: RDMA Write → 发送 CDC 信号 (post_send_cdc)
                           ↓
接收端: 收到 CDC → 知道数据已就绪，开始处理
                   处理完毕 → 发送 ACK (server_is_confirmed)
                           ↓
发送端: 收到 ACK → 知道接收端已处理完毕
```

项目主要包含了两个文件夹：
```bash
scp_rdma工具的主要实现在 scp_rdma文件夹下；rdma_trans_src只包含了socket+cdc的实现函数(如有需要可以联系我获得 rdma_cm 和 write with immediate的实现函数,这里暂未使用到)
```

## 快速开始

### 编译
```bash
cd scp_rdma
meson setup build
ninja -C build
```

### 安装到默认路径
如果不想安装到系统，默认编译生成到build路径下
```bash
cd build
./scp_rdma xx xx ..
```

### 安装到系统 
安装
```bash
chmod +x install.sh
./install.sh
```

卸载
```bash
./install.sh uninstall
```

### 自定义安装路径
```bash
./install.sh --prefix=$HOME/.local
```

## 使用方法

### 说明

由于rdma传输机制是直接传输到服务端（接收端）的内存中，为了安全起见，在服务端和客户端都设置了相对路径的功能.


**服务端 (接收文件):**

必须使用-s 表明为服务端，如果不使用 -o file_path 来指定发送来的文件保存在哪里，默认就当前目录 
```bash
scp_rdma -s 
```

可以使用 -o file_path 指定发来的文件保存在哪里(以 /tmp 为例，接受的文件保存到本机的 /tmp路径下)
```bash
scp_rdma -s -o /tmp
```

**客户端 (发送文件):**

可以传输单独文件(比如压缩包等，不需要使用 -r 参数)，如果传输数据类型的为文件夹，则必须指定 -r 参数：
```bash
scp_rdma <本地文件> <服务器IP>:<远程路径>

# 示例
scp_rdma myfile.txt 192.168.1.100:/test/
scp_rdma -r /home/test/file 192.168.1.100:/test/  # 递归传输目录
```
上面的192.168.1.100:/test/中 :/test/的作用也是为了指定保存的路径：

比如服务端设置了保存的路径为/tmp, 这里发送端指定了 /test/,那么最终文件在服务端保持的路径为 /tmp/test/myfile.txt

如果客户端不想做路径的修改，可以直接使用 ":"
```bash
# 示例
scp_rdma myfile.txt 192.168.1.100:
scp_rdma -r /home/test/file 192.168.1.100:  # 递归传输目录
```

## 选项

###  用户还可以指定使用哪张rdma网卡，以及接听的端口号

| 选项 | 说明 |
|------|------|
| `-s` | 服务端模式 |
| `-r` | 递归传输目录 |
| `-d <dev>` | 指定 RDMA 设备名 |
| `-p <port>` | TCP 端口 (默认 1777) |
| `-o <dir>` | 接收文件输出目录 |
| `-v` | 详细输出 |
| `-h` | 显示帮助 |

### 使用示例

接收端：

<img width="798" height="278" alt="cb5329d3b6844c7009992c13071533ed" src="https://github.com/user-attachments/assets/856dbc54-20c7-4d56-b552-f174b11d0607" />

发送端：

<img width="1147" height="238" alt="cd28eadd48795d9b84ea08b55586fd7e" src="https://github.com/user-attachments/assets/26d44e03-087b-4b7a-ac25-4f0c6f353619" />

