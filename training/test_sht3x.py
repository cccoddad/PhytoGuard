#!/usr/bin/env python3
"""Read SHT3X temperature/humidity sensor via I2C-0 on SS928 board.
   SHT3X-DIS has I2C address 0x44 (ADDR pin GND) or 0x45 (ADDR pin VDD).
   Run: ssh root@192.168.31.158 python3 < test_sht3x.py
   Or copy to board: scp test_sht3x.py root@192.168.31.158:/tmp/ && ssh root@192.168.31.158 python3 /tmp/test_sht3x.py
"""
"""
这是一个独立的板端传感器自检脚本。
它会轮流扫描 0x44 / 0x45 两个常见地址，确认 SHT3X 是否接线正确并能持续返回温湿度。
"""
import fcntl
import os
import struct
import time

# --- Constants ---
I2C_BUS = "/dev/i2c-0"
ADDR_44 = 0x44  # most common default
ADDR_45 = 0x45  # alternate (if ADDR pin is VDD)

# I2C ioctl defines (from linux/i2c-dev.h)
I2C_SLAVE = 0x0703
I2C_RETRIES = 0x0701
I2C_TIMEOUT = 0x0702

CMD_SINGLE_HIGH_REP = b'\x2C\x06'  # Single measurement, high repeatability
CMD_SOFT_RESET = b'\x30\xA2'       # Soft reset


def sht3x_read(addr):
    """Try reading SHT3X from a given I2C address. Returns (temp_c, rh_pct) or None."""
    fd = os.open(I2C_BUS, os.O_RDWR)
    try:
        # Set slave address
        fcntl.ioctl(fd, I2C_SLAVE, addr)
        fcntl.ioctl(fd, I2C_RETRIES, 2)
        fcntl.ioctl(fd, I2C_TIMEOUT, 50)

        # Send measurement command
        written = os.write(fd, CMD_SINGLE_HIGH_REP)
        if written != 2:
            return None

        # SHT3X needs ~15ms for high-repeatability measurement
        time.sleep(0.020)

        # Read 6 bytes: temp[15:0], crc, humidity[15:0], crc
        data = os.read(fd, 6)
        if len(data) < 6:
            return None

        raw_t = (data[0] << 8) | data[1]
        raw_h = (data[3] << 8) | data[4]

        temp_c = -45.0 + 175.0 * raw_t / 65535.0
        rh_pct = 100.0 * raw_h / 65535.0
        return (round(temp_c, 2), round(rh_pct, 2))
    except OSError as e:
        print(f"  I2C error: {e}")
        return None
    finally:
        os.close(fd)


def main():
    print("=== SHT3X Sensor Test ===")
    print(f"Bus: {I2C_BUS}")
    print()

    # Scan both possible addresses
    for addr, name in [(ADDR_44, "0x44 (default)"), (ADDR_45, "0x45 (alternate)")]:
        print(f"Scanning address {name}...")
        result = sht3x_read(addr)
        if result:
            t, rh = result
            print(f"  ✅ SHT3X FOUND at {hex(addr)}")
            print(f"     Temperature(温度): {t} °C")
            print(f"     Humidity(湿度):    {rh} %RH")
            print()
            # Continuous reading demo
            print("  Continuous read (5 samples, 2s interval):")
            for i in range(5):
                time.sleep(2)
                r2 = sht3x_read(addr)
                if r2:
                    print(f"    #{i+1}: {r2[0]}°C  {r2[1]}%RH")
            return
        else:
            print(f"  No device at {hex(addr)}")
            print()

    print("❌ SHT3X not found on I2C-0!")
    print("Check:")
    print("  1. VDD → 3.3V (not 5V!)")
    print("  2. GND → GND")
    print("  3. SDA → I2C0_SDA")
    print("  4. SCL → I2C0_SCL")
    print("  5. Run: i2cdetect -y -r 0")


if __name__ == "__main__":
    main()
