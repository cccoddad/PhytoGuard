/* SHT3X 温湿度传感器驱动头文件。 */
#ifndef SHT3X_H
#define SHT3X_H

/* 打开 I2C 设备并绑定到指定地址。 */
int sht3x_open(const char *i2c_bus, unsigned char addr);
/* 关闭传感器句柄。 */
void sht3x_close(void);
/* 判断当前传感器是否已经成功打开。 */
int sht3x_is_open(void);
/* 主动读取一次温湿度，成功后会刷新内部缓存。 */
int sht3x_read(float *temp_c_out, float *rh_pct_out);
/* 读取最近一次成功采样的缓存值，避免每次都阻塞访问 I2C。 */
void sht3x_get_last(float *temp_c_out, float *rh_pct_out);

#endif
