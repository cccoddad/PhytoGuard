#!/bin/sh
# L610 4G 拨号脚本（幂等）。
#
# 注意：本脚本依据 L610 常见 ECM 拨号流程编写，未经实机验证。
# 拿到模组 AT 手册后，请核对下方 L610_ECM_ACTIVATE 等指令是否与固件一致。
#
# 幂等语义：已存在 enx* 接口且已分配 IP 时直接退出，可被开机脚本/watchdog 反复调用。

# ---- 可配置参数（按现场环境修改）----
L610_AT_TTY="${L610_AT_TTY:-/dev/ttyUSB2}"   # AT 命令口
L610_APN="${L610_APN:-cmnet}"                # 运营商 APN
L610_ECM_IFACE="${L610_ECM_IFACE:-enx*}"     # ECM 网络接口名通配
L610_ECM_ACTIVATE="${L610_ECM_ACTIVATE:-AT+CGACT=1,1}"  # ECM 激活指令（占位，待实测）
LOG=/tmp/l610_connect.log

log() { echo "$(date '+%F %T') [l610] $*" >>"$LOG"; }

# 已有接口且已分配 IPv4 → 视为已连接。
for iface in $L610_ECM_IFACE; do
    [ -d "/sys/class/net/$iface" ] || continue
    ip=$(ip -4 addr show "$iface" 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1)
    if [ -n "$ip" ]; then
        log "already connected: $iface=$ip"
        exit 0
    fi
done

log "connecting (tty=$L610_AT_TTY apn=$L610_APN)"

# AT 口裸模式，避免回显干扰解析。
stty -F "$L610_AT_TTY" 115200 raw -echo 2>>"$LOG" || {
    log "open $L610_AT_TTY failed"
    exit 1
}

at() {
    # 发一条 AT 命令并等待 OK，失败返回非 0。
    printf '%s\r' "$1" >"$L610_AT_TTY"
    i=0
    while [ "$i" -lt 10 ]; do
        if timeout 1 cat "$L610_AT_TTY" 2>/dev/null | grep -q "OK"; then
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

at "AT" || { log "AT probe failed"; exit 1; }
at "AT+CGDCONT=1,\"IP\",\"$L610_APN\"" || log "CGDCONT failed (continuing)"
at "AT+CFUN=1" || log "CFUN=1 failed (continuing)"
at "$L610_ECM_ACTIVATE" || log "ECM activate failed"

# 轮询 PDP 激活状态。
i=0
while [ "$i" -lt 15 ]; do
    printf 'AT+CGACT?\r' >"$L610_AT_TTY"
    if timeout 1 cat "$L610_AT_TTY" 2>/dev/null | grep -q "+CGACT: 1,1"; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

# 等待 ECM 网口注册进系统。
i=0
while [ "$i" -lt 10 ]; do
    for iface in $L610_ECM_IFACE; do
        [ -d "/sys/class/net/$iface" ] && break 2
    done
    i=$((i + 1))
    sleep 1
done

# DHCP 获取地址。
for iface in $L610_ECM_IFACE; do
    [ -d "/sys/class/net/$iface" ] || continue
    udhcpc -i "$iface" -n -q >>"$LOG" 2>&1 && log "connected: $iface" && exit 0
done

log "connect failed"
exit 1
