# serial_i2c_temp_socket_driver

Linux 用户态示例程序：

1. 初始化串口（115200 8N1，可通过参数关闭）；
2. 通过 I2C 读取 TMP102/LM75 兼容温度寄存器（0x00）；
3. 通过 TCP Socket 将温度 JSON 上传到服务器。

## 编译

```bash
gcc -O2 -Wall -Wextra -o serial_driver serial_i2c_temp_socket_driver.c
```

## 运行示例

```bash
./serial_driver -d /dev/ttyS1 -i /dev/i2c-1 -a 0x48 -h 192.168.1.10 -p 9000 -t 5
```

## 参数

- `-d <serial_dev>`：串口设备（默认 `/dev/ttyS1`）
- `-i <i2c_dev>`：I2C 设备（默认 `/dev/i2c-1`）
- `-a <i2c_addr>`：I2C 从设备地址，支持十六进制（默认 `0x48`）
- `-h <server_ip>`：服务器 IP（默认 `127.0.0.1`）
- `-p <server_port>`：服务器端口（默认 `9000`）
- `-t <interval_sec>`：采样上报周期秒数（默认 `5`）
- `--no-serial`：禁用串口初始化和串口输出
- `--help`：显示帮助

## 上传数据格式

每次上传一行 JSON，例如：

```json
{"temperature_c": 26.50, "ts": 1739596800}
```

其中 `ts` 为 Unix 时间戳（秒）。
