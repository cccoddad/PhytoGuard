/*
 * SHT3X 温湿度传感器驱动。
 *
 * 设计目标：
 * 1. 不依赖额外库，直接通过 Linux I2C_RDWR ioctl 完成读写。
 * 2. 每次需要环境信息时做一次单次采样，避免常驻轮询线程。
 * 3. 保存最近一次成功读取的结果，供告警上报和结果 JSON 复用。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

/* 这里内联最小 I2C 结构体定义，避免交叉编译环境里头文件缺失。 */
#ifndef I2C_SLAVE
#define I2C_SLAVE    0x0703
#endif
#ifndef I2C_RDWR
#define I2C_RDWR     0x0707
#endif
#ifndef I2C_M_RD
#define I2C_M_RD     0x0001
#endif

struct i2c_msg {
    unsigned short addr;
    unsigned short flags;
    unsigned short len;
    unsigned char  *buf;
};

struct i2c_rdwr_ioctl_data {
    struct i2c_msg *msgs;
    int nmsgs;
};

static int g_fd = -1;
static unsigned char g_addr = 0x44;
static float g_last_temp_c = -999.0f;
static float g_last_rh_pct = -999.0f;

int sht3x_open(const char *i2c_bus, unsigned char addr)
{
    /* 打开总线后只记录一个全局句柄，当前工程中不需要多实例传感器。 */
    g_fd = open(i2c_bus, O_RDWR);
    if (g_fd < 0) {
        fprintf(stderr, "sht3x: open %s failed, errno=%d\n", i2c_bus, errno);
        return -1;
    }
    g_addr = addr;

    /* 先验证目标从设备地址确实可访问。 */
    if (ioctl(g_fd, I2C_SLAVE, g_addr) < 0) {
        fprintf(stderr, "sht3x: ioctl I2C_SLAVE 0x%02x failed, errno=%d\n",
                g_addr, errno);
        close(g_fd);
        g_fd = -1;
        return -1;
    }

    return 0;
}

void sht3x_close(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
}

int sht3x_is_open(void)
{
    return g_fd >= 0;
}

/*
 * 读取一次测量值。
 * 流程是：下发“高重复度单次测量”命令 -> 等待芯片完成采样 -> 再读回 6 字节结果。
 * 注意：这里没有校验 CRC，换来的是实现更简单；对实验项目通常够用。
 */
int sht3x_read(float *temp_c_out, float *rh_pct_out)
{
    unsigned char cmd[2] = {0x2C, 0x06};
    unsigned char data[6];
    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data rdwr;

    if (g_fd < 0) return -1;

    /* 第 1 步：写入 2 字节测量命令。 */
    msgs[0].addr  = g_addr;
    msgs[0].flags = 0;
    msgs[0].len   = 2;
    msgs[0].buf   = cmd;

    /* 第 2 步：准备读取 6 字节原始结果。 */
    msgs[1].addr  = g_addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = 6;
    msgs[1].buf   = data;

    rdwr.msgs  = msgs;
    rdwr.nmsgs = 2;

    /* 先触发测量。 */
    rdwr.msgs  = &msgs[0];
    rdwr.nmsgs = 1;
    if (ioctl(g_fd, I2C_RDWR, &rdwr) < 0) {
        fprintf(stderr, "sht3x: I2C_RDWR write failed, errno=%d\n", errno);
        return -1;
    }

    /* 高重复度模式需要约 15ms，这里稍微放宽到 20ms。 */
    usleep(20000);

    /* 再把温度和湿度原始值读回来。 */
    rdwr.msgs  = &msgs[1];
    rdwr.nmsgs = 1;
    if (ioctl(g_fd, I2C_RDWR, &rdwr) < 0) {
        fprintf(stderr, "sht3x: I2C_RDWR read failed, errno=%d\n", errno);
        return -1;
    }

    /* 按芯片手册公式把原始值换算为摄氏度和相对湿度。 */
    {
        uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
        uint16_t raw_h = ((uint16_t)data[3] << 8) | data[4];

        *temp_c_out = -45.0f + 175.0f * (float)raw_t / 65535.0f;
        *rh_pct_out = 100.0f * (float)raw_h / 65535.0f;
    }

    /* 缓存最近一次结果，便于其他模块读取。 */
    g_last_temp_c = *temp_c_out;
    g_last_rh_pct = *rh_pct_out;

    return 0;
}

void sht3x_get_last(float *temp_c_out, float *rh_pct_out)
{
    /* 即使当前传感器暂时掉线，也能返回最后一次成功采样的值。 */
    *temp_c_out = g_last_temp_c;
    *rh_pct_out = g_last_rh_pct;
}
