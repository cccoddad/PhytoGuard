#!/bin/sh
# L610 4G 链路守护进程。
#
# 注意：未经实机验证，依赖 l610_connect.sh（参数见该脚本头部）。
# 每 30 秒做四段检测：接口存在 → AT 口可读 → 有 IP → ping 通公网。
# 任一失败即调用 l610_connect.sh 重连；连续失败时退避到 60 秒。

CONNECT="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/l610_connect.sh"
L610_AT_TTY="${L610_AT_TTY:-/dev/ttyUSB2}"
L610_ECM_IFACE="${L610_ECM_IFACE:-enx*}"
PING_TARGET="${L610_PING_TARGET:-223.5.5.5}"
LOG=/tmp/l610_watchdog.log
PIDFILE=/var/run/l610_watchdog.pid
INTERVAL=30
BACKOFF_INTERVAL=60

log() { echo "$(date '+%F %T') [watchdog] $*" >>"$LOG"; }

# 防止重复拉起。
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "l610_watchdog already running (pid $(cat "$PIDFILE"))"
    exit 0
fi
echo $$ >"$PIDFILE"

# TERM/INT/EXIT 统一清理 pid 文件，风格与 run_attach_two_stage.sh 一致。
cleanup() {
    rm -f "$PIDFILE"
    exit 0
}
trap cleanup TERM INT EXIT

fail=0
while :; do
    dead=0

    # 1) 接口存在
    iface_found=""
    for iface in $L610_ECM_IFACE; do
        [ -d "/sys/class/net/$iface" ] && iface_found=$iface && break
    done
    [ -n "$iface_found" ] || { dead=1; log "iface missing"; }

    # 2) AT 口可读
    [ -r "$L610_AT_TTY" ] || { dead=1; log "AT tty $L610_AT_TTY unreadable"; }

    # 3) 已有 IPv4
    if [ "$dead" -eq 0 ] && [ -n "$iface_found" ]; then
        ip=$(ip -4 addr show "$iface_found" 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1)
        [ -n "$ip" ] || { dead=1; log "no IPv4 on $iface_found"; }
    fi

    # 4) 公网连通
    if [ "$dead" -eq 0 ]; then
        ping -c 3 -W 3 "$PING_TARGET" >/dev/null 2>&1 || { dead=1; log "ping $PING_TARGET failed"; }
    fi

    if [ "$dead" -eq 1 ]; then
        fail=$((fail + 1))
        log "link down (streak=$fail), reconnecting..."
        sh "$CONNECT" >>"$LOG" 2>&1
        # 连续失败时退避，避免疯狂重连把模组打挂。
        if [ "$fail" -ge 3 ]; then
            sleep "$BACKOFF_INTERVAL"
        else
            sleep "$INTERVAL"
        fi
    else
        fail=0
        sleep "$INTERVAL"
    fi
done
