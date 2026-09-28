/*
 * 单设备告警页。
 *
 * 这是告警总览页的一个“设备过滤版本”，
 * 逻辑上仍然支持轮询刷新、已读、删除和筛选。
 */
const api = require('../../utils/api');

Page({
  data: {
    allAlerts: [],
    alerts: [],
    page: 1, limit: 20,
    total: 0, unread: 0, todayCount: 0,
    loading: false, hasMore: true, error: '',
    deviceId: '', filter: 'all', pageTitle: '',
    selectMode: false, selectedIds: {}, selectedCount: 0,
    touchStartX: 0, touchStartY: 0
  },

  onLoad(options) {
    const deviceId = (options && options.device_id) || '';
    this.setData({
      deviceId: deviceId,
      pageTitle: deviceId ? '设备 ' + deviceId : '设备告警'
    });
    this.refreshAlerts();
    this.startPolling();
  },

  onUnload() { this.stopPolling(); },

  // ---- Swipe right to go back ----
  onTouchStart(e) { this.data.touchStartX = e.touches[0].pageX; this.data.touchStartY = e.touches[0].pageY; },
  onTouchEnd(e) {
    const dx = e.changedTouches[0].pageX - this.data.touchStartX;
    const dy = Math.abs(e.changedTouches[0].pageY - this.data.touchStartY);
    if (dx > 80 && dx > dy * 1.5) wx.navigateBack();
  },

  onPullDownRefresh() { this.exitSelectMode(); this.refreshAlerts(); },
  onReachBottom() { if (!this.data.hasMore || this.data.loading || this.data.selectMode) return; this.loadAlerts(); },

  startPolling() { this._timer = setInterval(() => { if (!this.data.selectMode) this.refreshAlerts(); }, 30000); },
  stopPolling() { if (this._timer) { clearInterval(this._timer); this._timer = null; } },

  loadAlerts(reset) {
    // 所有请求都会固定带上当前 deviceId，只拉该设备的告警。
    if (this.data.loading) return;
    this.setData({ loading: true, error: '' });
    const page = reset ? 1 : this.data.page;
    Promise.all([
      api.getAlerts(page, this.data.limit, this.data.deviceId),
      api.getStats(this.data.deviceId).catch(() => null)
    ]).then(([data, stats]) => {
      const rows = Array.isArray(data.rows) ? data.rows : [];
      const alerts = reset ? rows : this.data.alerts.concat(rows);
      const prepared = this.prepareAlerts(alerts);
      const fs = this.getFallbackStats(prepared, data);
      const allAlerts = reset ? prepared : this.data.allAlerts.concat(prepared);
      for (const a of allAlerts) a._checked = this.data.selectedIds[a.id] ? true : false;
      this.setData({
        allAlerts, page: page + 1, total: stats ? Number(stats.totalCount) : fs.total,
        unread: stats ? Number(stats.unreadCount) : fs.unread,
        todayCount: stats ? Number(stats.todayCount) : fs.todayCount,
        loading: false, hasMore: allAlerts.length < (stats ? Number(stats.totalCount) : fs.total), error: ''
      });
      this.applyFilter(); wx.stopPullDownRefresh();
    }).catch(err => {
      this.setData({ loading: false, error: (err && err.msg) || '网络请求失败' }); wx.stopPullDownRefresh();
    });
  },

  refreshAlerts() { this.setData({ page: 1, hasMore: true, error: '' }); this.loadAlerts(true); },

  // ---- Top button ----
  onTopBtn() {
    if (this.data.selectMode) { this.exitSelectMode(); return; }
    wx.showModal({
      title: '标记全部已读',
      content: '将 ' + (this.data.deviceId || '所有') + ' 的未读告警全部标记为已读？',
      success: (res) => { if (res.confirm) this.markAllRead(); }
    });
  },

  // ---- Multi-select ----
  enterSelectMode() { this.setData({ selectMode: true, selectedIds: {}, selectedCount: 0 }); },
  exitSelectMode() {
    for (const a of this.data.alerts) a._checked = false;
    this.setData({ selectMode: false, selectedIds: {}, selectedCount: 0 });
  },
  onToggleSelect(e) {
    const id = e.currentTarget.dataset.id;
    const selectedIds = { ...this.data.selectedIds };
    selectedIds[id] ? delete selectedIds[id] : selectedIds[id] = true;
    let count = 0;
    for (const a of this.data.alerts) { a._checked = !!selectedIds[a.id]; if (a._checked) count++; }
    this.setData({ selectedIds, selectedCount: count });
  },
  onSelectAll() {
    const selectedIds = {};
    for (const a of this.data.alerts) { a._checked = true; selectedIds[a.id] = true; }
    this.setData({ selectedIds, selectedCount: this.data.alerts.length });
  },
  onDeleteSelected() {
    const ids = Object.keys(this.data.selectedIds);
    if (ids.length === 0) { wx.showToast({ title: '请先选择', icon: 'none' }); return; }
    wx.showModal({
      title: '删除告警', content: '确定删除选中的 ' + ids.length + ' 条告警？',
      success: (res) => {
        if (!res.confirm) return;
        wx.showLoading({ title: '删除中...' });
        let done = 0;
        ids.forEach((id, i) => {
          api.deleteAlert(id).then(() => { done++; }).catch(() => {}).finally(() => {
            if (i === ids.length - 1) { wx.hideLoading(); this.exitSelectMode(); this.refreshAlerts(); }
          });
        });
      }
    });
  },

  // ---- Long press ----
  onLongPress(e) {
    const id = e.currentTarget.dataset.id;
    if (!id) return;
    wx.showActionSheet({
      itemList: ['标记已读', '删除'],
      success: (res) => {
        if (res.tapIndex === 0) {
          api.acknowledgeAlert(id).then(() => { wx.showToast({ title: '已读', icon: 'success' }); this.refreshAlerts(); });
        } else {
          wx.showModal({
            title: '确认删除', content: '确定删除？',
            success: (r) => { if (r.confirm) api.deleteAlert(id).then(() => { wx.showToast({ title: '已删除', icon: 'success' }); this.refreshAlerts(); }); }
          });
        }
      }
    });
  },

  markAllRead() {
    const unread = this.data.allAlerts.filter(a => !a.acknowledged);
    if (unread.length === 0) { wx.showToast({ title: '没有未读', icon: 'none' }); return; }
    wx.showLoading({ title: '标记中...' });
    let done = 0;
    unread.forEach((a, i) => {
      api.acknowledgeAlert(a.id).then(() => { done++; }).catch(() => {}).finally(() => {
        if (i === unread.length - 1) { wx.hideLoading(); wx.showToast({ title: '已标记' + done + '条', icon: 'success' }); this.refreshAlerts(); }
      });
    });
  },

  goDetail(e) { if (this.data.selectMode) return; const id = e.currentTarget.dataset.id; if (id) wx.navigateTo({ url: '/pages/detail/detail?id=' + id }); },
  onStatTap(e) { const k = e.currentTarget.dataset.key; const n = (this.data.filter === k) ? 'all' : k; this.setData({ filter: n }); this.applyFilter(); },
  applyFilter() {
    let list = this.data.allAlerts;
    if (this.data.filter === 'unread') list = list.filter(a => !a.acknowledged);
    if (this.data.filter === 'today') { const s = new Date().setHours(0,0,0,0); list = list.filter(a => a.ts_ms >= s); }
    list = list.map(a => ({ ...a, _checked: !!this.data.selectedIds[a.id] }));
    this.setData({ alerts: list });
  },

  prepareAlerts(alerts) {
    // 与总告警页保持相同的数据映射规则，保证交互和展示一致。
    return alerts.map(alert => {
      const prob = Number(alert.top1_prob) || 0;
      return {
        ...alert,
        displayName: (alert.crop_name && alert.disease_name) ? (alert.crop_name + alert.disease_name) : (alert.top1_label_zh || alert.top1_label || '未知病害'),
        deviceName: alert.device_id || '未知设备',
        probPercent: (prob * 100).toFixed(0),
        severityClass: prob >= 0.85 ? 'severe' : (prob >= 0.70 ? 'warning' : 'normal'),
        acknowledged: Number(alert.acknowledged) === 1,
        cardClass: Number(alert.acknowledged) === 1 ? '' : 'unread',
        statusClass: Number(alert.acknowledged) === 1 ? 'done' : 'new',
        statusText: Number(alert.acknowledged) === 1 ? '已读' : '未读',
        envText: ((alert.temperature_c != null && alert.temperature_c != -999) ? (Number(alert.temperature_c).toFixed(1) + '°C / ' + Number(alert.humidity_rh).toFixed(1) + '%') : '')
      };
    });
  },
  getFallbackStats(alerts, data) {
    const s = new Date().setHours(0,0,0,0);
    return { total: Number(data.total) || alerts.length, unread: alerts.filter(i => !i.acknowledged).length, todayCount: alerts.filter(i => i.ts_ms >= s).length };
  }
});
