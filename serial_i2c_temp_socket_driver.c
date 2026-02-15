#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SERIAL_DEVICE "/dev/ttyS1"
#define DEFAULT_I2C_DEVICE "/dev/i2c-1"
#define DEFAULT_I2C_ADDR 0x48
#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 9000
#define DEFAULT_INTERVAL_SEC 5

static volatile sig_atomic_t g_running = 1;

struct app_config {
    const char *serial_dev;
    const char *i2c_dev;
    int i2c_addr;
    const char *server_ip;
    int server_port;
    int interval_sec;
    int enable_serial;
};

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = write(fd, buf + offset, len - offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = send(fd, buf + offset, len - offset, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

/*
 * 初始化串口：115200 8N1，无流控。
 */
static int init_serial_port(const char *serial_dev) {
    int fd = open(serial_dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~IGNBRK;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

static int init_i2c_bus(const char *i2c_dev, int i2c_addr) {
    int fd = open(i2c_dev, O_RDWR);
    if (fd < 0) {
        perror("open i2c");
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, i2c_addr) < 0) {
        perror("ioctl I2C_SLAVE");
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * 读取 TMP102/LM75 兼容温度寄存器 0x00。
 */
static int read_temperature_celsius(int i2c_fd, float *temp_c) {
    if (!temp_c) {
        errno = EINVAL;
        return -1;
    }

    uint8_t reg = 0x00;
    if (write(i2c_fd, &reg, 1) != 1) {
        perror("write i2c reg");
        return -1;
    }

    uint8_t data[2] = {0};
    if (read(i2c_fd, data, 2) != 2) {
        perror("read i2c data");
        return -1;
    }

    int16_t raw = (int16_t)((data[0] << 8) | data[1]);
    raw >>= 4;
    if (raw & 0x0800) {
        raw |= 0xF000;
    }

    *temp_c = raw * 0.0625f;
    return 0;
}

static int send_temperature_to_server(const char *server_ip, int server_port, float temp_c) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    time_t now = time(NULL);
    char payload[192];
    int n = snprintf(payload,
                     sizeof(payload),
                     "{\"temperature_c\": %.2f, \"ts\": %ld}\n",
                     temp_c,
                     (long)now);
    if (n < 0 || n >= (int)sizeof(payload)) {
        fprintf(stderr, "payload format error\n");
        close(sockfd);
        return -1;
    }

    if (send_all(sockfd, payload, (size_t)n) < 0) {
        perror("send");
        close(sockfd);
        return -1;
    }

    close(sockfd);
    return 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -d <serial_dev>   串口设备（默认: %s）\n"
            "  -i <i2c_dev>      I2C 设备（默认: %s）\n"
            "  -a <i2c_addr>     I2C 从设备地址，支持 0x48（默认: 0x%X）\n"
            "  -h <server_ip>    服务器 IP（默认: %s）\n"
            "  -p <server_port>  服务器端口（默认: %d）\n"
            "  -t <interval>     采集间隔秒（默认: %d）\n"
            "  --no-serial       禁用串口初始化与串口日志输出\n"
            "  --help            显示帮助\n"
            "示例: %s -d /dev/ttyS1 -i /dev/i2c-1 -a 0x48 -h 192.168.1.10 -p 9000 -t 5\n",
            prog,
            DEFAULT_SERIAL_DEVICE,
            DEFAULT_I2C_DEVICE,
            DEFAULT_I2C_ADDR,
            DEFAULT_SERVER_IP,
            DEFAULT_SERVER_PORT,
            DEFAULT_INTERVAL_SEC,
            prog);
}

static int parse_args(int argc, char *argv[], struct app_config *cfg) {
    if (!cfg) {
        return -1;
    }

    cfg->serial_dev = DEFAULT_SERIAL_DEVICE;
    cfg->i2c_dev = DEFAULT_I2C_DEVICE;
    cfg->i2c_addr = DEFAULT_I2C_ADDR;
    cfg->server_ip = DEFAULT_SERVER_IP;
    cfg->server_port = DEFAULT_SERVER_PORT;
    cfg->interval_sec = DEFAULT_INTERVAL_SEC;
    cfg->enable_serial = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--no-serial") == 0) {
            cfg->enable_serial = 0;
            continue;
        }

        if (i + 1 >= argc) {
            fprintf(stderr, "参数缺少值: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "-d") == 0) {
            cfg->serial_dev = argv[++i];
        } else if (strcmp(argv[i], "-i") == 0) {
            cfg->i2c_dev = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            cfg->i2c_addr = (int)strtol(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-h") == 0) {
            cfg->server_ip = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            cfg->server_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0) {
            cfg->interval_sec = atoi(argv[++i]);
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return -1;
        }
    }

    if (cfg->server_port <= 0 || cfg->server_port > 65535 || cfg->interval_sec <= 0 ||
        cfg->i2c_addr < 0x03 || cfg->i2c_addr > 0x77) {
        fprintf(stderr, "参数范围无效\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    struct app_config cfg;
    int parse_ret = parse_args(argc, argv, &cfg);
    if (parse_ret != 0) {
        if (parse_ret < 0) {
            print_usage(argv[0]);
            return 1;
        }
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int serial_fd = -1;
    if (cfg.enable_serial) {
        serial_fd = init_serial_port(cfg.serial_dev);
        if (serial_fd >= 0) {
            const char *msg = "serial initialized\n";
            if (write_all(serial_fd, msg, strlen(msg)) < 0) {
                perror("write serial");
            }
        } else {
            fprintf(stderr, "串口初始化失败，继续执行 I2C + Socket 主流程\n");
        }
    }

    int i2c_fd = init_i2c_bus(cfg.i2c_dev, cfg.i2c_addr);
    if (i2c_fd < 0) {
        if (serial_fd >= 0) {
            close(serial_fd);
        }
        return 1;
    }

    while (g_running) {
        float temp_c = 0.0f;
        if (read_temperature_celsius(i2c_fd, &temp_c) == 0) {
            printf("当前温度: %.2f C\n", temp_c);
            if (send_temperature_to_server(cfg.server_ip, cfg.server_port, temp_c) == 0) {
                printf("上传成功: %.2f C -> %s:%d\n", temp_c, cfg.server_ip, cfg.server_port);
            } else {
                fprintf(stderr, "上传失败\n");
            }

            if (serial_fd >= 0) {
                char serial_msg[96];
                int n = snprintf(serial_msg, sizeof(serial_msg), "temp=%.2fC\n", temp_c);
                if (n > 0 && n < (int)sizeof(serial_msg)) {
                    if (write_all(serial_fd, serial_msg, (size_t)n) < 0) {
                        perror("write serial");
                    }
                }
            }
        } else {
            fprintf(stderr, "读取温度失败\n");
        }

        sleep((unsigned int)cfg.interval_sec);
    }

    close(i2c_fd);
    if (serial_fd >= 0) {
        close(serial_fd);
    }

    printf("程序已退出。\n");
    return 0;
}
