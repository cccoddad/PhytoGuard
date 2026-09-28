/*
 * 设备状态页。
 *
 * 这里集中展示每台设备的在线状态、最近心跳、告警数量、
 * 内存占用以及最近一条附带的温湿度信息。
 */
const api = require('../../utils/api');
Page({
  data: {
    allDevices: [],       // raw list from backend
    devices: [],          // filtered list for display
    totalCount: 0,
    onlineCount: 0,
    offlineCount: 0,
    filter: 'all',        // 'all' | 'online' | 'offline'
    loading: false,
    error: ''
  },

  onLoad() {
    this.loadDevices();
    this.startPolling();
  },

  onShow() {
    // 从注册页返回时立即刷新，否则要等下一轮 30 秒轮询才能看到新设备。
    if (this._needsRefresh) {
      this._needsRefresh = false;
      this.loadDevices();
    }
  },

  onUnload() {
    this.stopPolling();
  },

  onPullDownRefresh() {
    this.loadDevices();
  },

  startPolling() {
    this._timer = setInterval(() => this.loadDevices(), 30000);
  },

  stopPolling() {
    if (this._timer) { clearInterval(this._timer); this._timer = null; }
  },

  loadDevices() {
    // 设备列表接口已经做过后端聚合，前端主要负责映射和筛选展示。
    this.setData({ loading: true, error: '' });

    api.getDevices()
      .then(devices => {
        const list = Array.isArray(devices) ? devices : [];
        const prepared = this.prepareDevices(list);
        this.setData({
          allDevices: prepared,
          totalCount: prepared.length,
          onlineCount: prepared.filter(d => d.online).length,
          offlineCount: prepared.filter(d => !d.online).length,
          loading: false,
          error: ''
        });
        this.applyFilter();
        wx.stopPullDownRefresh();
      })
      .catch(err => {
        console.error('load devices failed:', err);
        this.setData({
          loading: false,
          error: (err && err.msg) || '网络请求失败'
        });
        wx.stopPullDownRefresh();
      });
  },

  // ---- stat bar taps ----
  onRegisterTap() {
    wx.navigateTo({ url: '/pages/register/register' });
  },

  goDeviceAlerts(e) {
    const deviceId = (e && e.currentTarget && e.currentTarget.dataset.deviceId) ||
                     (e && e.detail && e.detail.device_id);
    if (deviceId) {
      wx.navigateTo({ url: '/pages/device-alerts/device-alerts?device_id=' + deviceId });
    }
  },

  onStatTap(e) {
    const key = e.currentTarget.dataset.key;
    // toggle filter: click same again → back to 'all'
    const next = (this.data.filter === key) ? 'all' : key;
    this.setData({ filter: next });
    this.applyFilter();
  },

  applyFilter() {
    const filter = this.data.filter;
    let list = this.data.allDevices;
    if (filter === 'online') {
      list = list.filter(d => d.online);
    } else if (filter === 'offline') {
      list = list.filter(d => !d.online);
    } else {
      // show all: online first, then offline
      list = [...list].sort((a, b) => (b.online ? 1 : 0) - (a.online ? 1 : 0));
    }
    this.setData({ devices: list });
  },

  // ---- device data mapping ----
  prepareDevices(devices) {
    // 兼容不同版本后端字段，并派生出页面直接使用的展示文案。
    return devices.map(device => {
      const memoryPercent = this.getNumber(device.memory_percent, device.mem_percent, device.memory_usage);
      const temperature = this.getNumber(device.temperature, device.temp, device.temperature_c);
      const humidity = this.getNumber(device.humidity_rh, device.humidity, device.humidity_percent);
      const lastSeenTs = this.getNumber(device.last_seen_ts, device.last_status_ts, device.updated_at_ms);
      const online = this.getOnlineState(device, lastSeenTs);

      return {
        ...device,
        displayName: device.name || device.device_id || '未命名设备',
        locationText: device.location || '未设置位置',
        online: online,
        onlineClass: online ? 'online' : 'offline',
        onlineText: online ? '运行中' : '离线',
        bootText: this.getBootText(device, online),
        memoryText: memoryPercent === null ? '待接入' : memoryPercent + '%',
        memoryClass: memoryPercent === null ? 'muted' : (memoryPercent >= 85 ? 'danger' : (memoryPercent >= 70 ? 'warn' : 'ok')),
        temperatureText: temperature === null ? '待接入' : temperature + '°C',
        humidityText: humidity === null ? '待接入' : humidity + '%',
        lastSeenTs: lastSeenTs || device.last_alert_ts || 0,
        alertCount: Number(device.alert_count) || 0,
        countClass: Number(device.alert_count) > 0 ? 'hot' : ''
      };
    });
  },

  getNumber(...values) {
    for (let i = 0; i < values.length; i += 1) {
      const value = Number(values[i]);
      if (!Number.isNaN(value) && values[i] !== null && values[i] !== undefined && values[i] !== '') {
        return Math.round(value);
      }
    }
    return null;
  },

  getOnlineState(device, lastSeenTs) {
    // Trust last_seen_ts over explicit online flag — a dead board can't send heartbeats
    if (lastSeenTs && Date.now() - lastSeenTs > 120000) return false;
    if (lastSeenTs && Date.now() - lastSeenTs <= 120000) return true;
    if (device.online !== undefined) return Boolean(device.online);
    if (device.powered_on !== undefined) return Boolean(device.powered_on);
    if (device.status) return device.status === 'online' || device.status === 'running';
    return false;
  },

  getBootText(device, online) {
    if (device.powered_on !== undefined) {
      return device.powered_on ? '已开机' : '未开机';
    }
    if (device.booted !== undefined) {
      return device.booted ? '已开机' : '未开机';
    }
    return online ? '已开机' : '待确认';
  }
});
