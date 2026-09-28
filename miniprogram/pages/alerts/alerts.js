/*
 * 告警总览页。
 *
 * 主要职责：
 * 1. 拉取告警列表与统计数据。
 * 2. 支持按“全部 / 今日 / 未读”筛选。
 * 3. 支持批量删除、单条已读、设备维度跳转。
 * 4. 通过轮询保持页面接近实时状态。
 */
const api = require('../../utils/api');

Page({
  data: {
    allAlerts: [],          // unfiltered full list
    alerts: [],             // filtered display list
    page: 1,
    limit: 20,
    total: 0,
    unread: 0,
    todayCount: 0,
    loading: false,
    hasMore: true,
    error: '',
    deviceId: '',
    filter: 'all',          // 'all' | 'today' | 'unread'
    pageTitle: '全部告警',
    // Multi-select mode
    selectMode: false,
    selectedIds: {},        // { id: true }
    selectedCount: 0,
    // Swipe tracking
    touchStartX: 0,
    touchStartY: 0
  },

  onLoad(options) {
    // 如果带了 device_id，就复用这套页面逻辑展示单设备告警。
    const deviceId = (options && (options.device_id || options.device)) || '';
    this.setFilter(deviceId);
    this.refreshAlerts();
    this.startPolling();
  },

  onShow() {
    if (this._needsRefresh) {
      this._needsRefresh = false;
      this.refreshAlerts();
    }
  },

  onUnload() {
    this.stopPolling();
  },

  onPullDownRefresh() {
    this.exitSelectMode();
    this.refreshAlerts();
  },

  onTabItemTap() {
    this.exitSelectMode();
  },

  onReachBottom() {
    if (!this.data.hasMore || this.data.loading || this.data.selectMode) return;
    this.loadAlerts();
  },

  // ---- Swipe-right to go back ----
  onTouchStart(e) {
    this.data.touchStartX = e.touches[0].pageX;
    this.data.touchStartY = e.touches[0].pageY;
  },

  onTouchEnd(e) {
    const dx = e.changedTouches[0].pageX - this.data.touchStartX;
    const dy = Math.abs(e.changedTouches[0].pageY - this.data.touchStartY);
    // swipe right > 80px, horizontal dominant
    if (dx > 80 && dx > dy * 1.5) {
      if (this.data.selectMode) {
        this.exitSelectMode();
      } else {
        wx.switchTab({ url: '/pages/devices/devices' });
      }
    }
  },

  // ---- Polling ----
  startPolling() {
    this._timer = setInterval(() => {
      if (!this.data.selectMode) this.refreshAlerts();
    }, 30000);
  },

  stopPolling() {
    if (this._timer) { clearInterval(this._timer); this._timer = null; }
  },

  // ---- Load data ----
  loadAlerts(reset) {
    // reset=true 表示重新加载第一页；否则视为滚动分页加载。
    if (this.data.loading) return;
    this.setData({ loading: true, error: '' });

    const page = reset ? 1 : this.data.page;
    const deviceId = this.data.deviceId;

    Promise.all([
      api.getAlerts(page, this.data.limit, deviceId),
      api.getStats(deviceId).catch(() => null)
    ])
      .then(([data, stats]) => {
        const rows = Array.isArray(data.rows) ? data.rows : [];
        const alerts = reset ? rows : this.data.alerts.concat(rows);
        const preparedAlerts = this.prepareAlerts(alerts);
        const fallbackStats = this.getFallbackStats(preparedAlerts, data);
        const total = stats ? Number(stats.totalCount) : fallbackStats.total;
        const unread = stats ? Number(stats.unreadCount) : fallbackStats.unread;
        const todayCount = stats ? Number(stats.todayCount) : fallbackStats.todayCount;
        const allAlerts = reset ? preparedAlerts : this.data.allAlerts.concat(preparedAlerts);

        // Preserve selection state
        const selectedIds = this.data.selectedIds;
        for (const a of allAlerts) {
          a._checked = selectedIds && selectedIds[a.id] ? true : false;
        }

        this.setData({
          allAlerts: allAlerts,
          page: page + 1,
          total: total, unread: unread, todayCount: todayCount,
          loading: false, hasMore: allAlerts.length < total, error: ''
        });
        this.applyFilter();

        if (unread > 0) {
          wx.setTabBarBadge({ index: 0, text: unread > 99 ? '99+' : String(unread) });
        } else {
          wx.removeTabBarBadge({ index: 0 });
        }
        wx.stopPullDownRefresh();
      })
      .catch(err => {
        console.error('load alerts failed:', err);
        this.setData({ loading: false, error: (err && err.msg) || '网络请求失败，请确认后端已启动' });
        wx.stopPullDownRefresh();
      });
  },

  refreshAlerts() {
    this.setData({ page: 1, hasMore: true, error: '' });
    this.loadAlerts(true);
  },

  // ---- Navigation ----
  goDetail(e) {
    if (this.data.selectMode) return;
    const id = e.currentTarget.dataset.id;
    if (id) {
      this._needsRefresh = true;
      wx.navigateTo({ url: '/pages/detail/detail?id=' + id });
    }
  },

  setFilter(deviceId) {
    this.setData({
      deviceId: deviceId || '',
      pageTitle: deviceId ? '设备 ' + deviceId : '全部告警',
      selectMode: false, selectedIds: {}, selectedCount: 0
    });
  },

  // ---- Top button: mark all read (when filtering device) or batch actions ----
  onTopBtn() {
    if (this.data.selectMode) {
      this.exitSelectMode();
      return;
    }
    if (this.data.deviceId) {
      // Confirm mark all unread as read for this device
      wx.showModal({
        title: '标记全部已读',
        content: '将该设备的所有未读告警标记为已读？',
        success: (res) => {
          if (res.confirm) this.markAllRead();
        }
      });
    } else {
      // Enter select mode
      this.enterSelectMode();
    }
  },

  // ---- Multi-select ----
  enterSelectMode() {
    const alerts = this.data.alerts.map(a => ({ ...a, _checked: false }));
    this.setData({ selectMode: true, alerts: alerts, selectedIds: {}, selectedCount: 0 });
  },

  exitSelectMode() {
    const alerts = this.data.alerts.map(a => ({ ...a, _checked: false }));
    this.setData({ selectMode: false, alerts: alerts, selectedIds: {}, selectedCount: 0 });
  },

  onToggleSelect(e) {
    const id = e.currentTarget.dataset.id;
    const alerts = this.data.alerts;
    const selectedIds = { ...this.data.selectedIds };

    if (selectedIds[id]) {
      delete selectedIds[id];
    } else {
      selectedIds[id] = true;
    }

    let count = 0;
    for (const a of alerts) {
      a._checked = !!selectedIds[a.id];
      if (a._checked) count++;
    }

    this.setData({ alerts: alerts, selectedIds: selectedIds, selectedCount: count });
  },

  onSelectAll() {
    const alerts = this.data.alerts;
    const selectedIds = {};
    for (const a of alerts) {
      a._checked = true;
      selectedIds[a.id] = true;
    }
    this.setData({ alerts: alerts, selectedIds: selectedIds, selectedCount: alerts.length });
  },

  onDeleteSelected() {
    const ids = Object.keys(this.data.selectedIds);
    if (ids.length === 0) {
      wx.showToast({ title: '请先选择告警', icon: 'none' });
      return;
    }
    wx.showModal({
      title: '删除告警',
      content: '确定删除选中的 ' + ids.length + ' 条告警？此操作不可撤销。',
      success: (res) => {
        if (!res.confirm) return;
        let done = 0;
        let fail = 0;
        const total = ids.length;
        wx.showLoading({ title: '删除中...' });
        ids.forEach((id, i) => {
          api.deleteAlert(id)
            .then(() => { done++; })
            .catch(() => { fail++; })
            .finally(() => {
              if (i === total - 1) {
                wx.hideLoading();
                wx.showToast({ title: '删除' + done + '条' + (fail ? ('，' + fail + '条失败') : ''), icon: 'none' });
                this.exitSelectMode();
                this.refreshAlerts();
              }
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
          api.acknowledgeAlert(id).then(() => {
            wx.showToast({ title: '已标记已读', icon: 'success' });
            this._needsRefresh = true;
            this.refreshAlerts();
          }).catch(() => wx.showToast({ title: '操作失败', icon: 'none' }));
        } else if (res.tapIndex === 1) {
          wx.showModal({
            title: '确认删除',
            content: '确定删除该告警？',
            success: (r) => {
              if (!r.confirm) return;
              api.deleteAlert(id).then(() => {
                wx.showToast({ title: '已删除', icon: 'success' });
                this.refreshAlerts();
              }).catch(() => wx.showToast({ title: '删除失败', icon: 'none' }));
            }
          });
        }
      }
    });
  },

  // ---- Mark all read ----
  markAllRead() {
    const unreadAlerts = this.data.allAlerts.filter(a => !a.acknowledged);
    if (unreadAlerts.length === 0) {
      wx.showToast({ title: '没有未读告警', icon: 'none' });
      return;
    }
    wx.showLoading({ title: '标记中...' });
    let done = 0;
    const total = unreadAlerts.length;
    unreadAlerts.forEach((a, i) => {
      api.acknowledgeAlert(a.id)
        .then(() => { done++; })
        .catch(() => {})
        .finally(() => {
          if (i === total - 1) {
            wx.hideLoading();
            wx.showToast({ title: '已标记' + done + '条已读', icon: 'success' });
            this.refreshAlerts();
          }
        });
    });
  },

  // ---- Stats filter ----
  onStatTap(e) {
    if (this.data.selectMode) return;
    const key = e.currentTarget.dataset.key;
    if (key === 'device') {
      wx.switchTab({ url: '/pages/devices/devices' });
      return;
    }
    const next = (this.data.filter === key) ? 'all' : key;
    this.setData({ filter: next });
    if (!this.data.deviceId) this.updatePageTitle();
    this.applyFilter();
  },

  applyFilter() {
    const filter = this.data.filter;
    let list = this.data.allAlerts;
    if (filter === 'unread') {
      list = list.filter(a => !a.acknowledged);
    } else if (filter === 'today') {
      const todayStart = new Date().setHours(0, 0, 0, 0);
      list = list.filter(a => a.ts_ms >= todayStart);
    }
    // Apply _checked state
    const selectedIds = this.data.selectedIds;
    list = list.map(a => ({ ...a, _checked: !!selectedIds[a.id] }));
    this.setData({ alerts: list });
  },

  updatePageTitle() {
    const titles = { all: '全部告警', today: '今日新增', unread: '未处理告警' };
    this.setData({ pageTitle: titles[this.data.filter] || '全部告警' });
  },

  // ---- Data mapping ----
  prepareAlerts(alerts) {
    // 把后端原始字段映射成更适合页面渲染的字段。
    return alerts.map(alert => {
      const prob = Number(alert.top1_prob) || 0;
      return {
        ...alert,
        displayName: (alert.crop_name && alert.disease_name) ? (alert.crop_name + alert.disease_name) : (alert.top1_label_zh || alert.top1_label || '未知病害'),
        cropName: alert.crop_name || '',
        deviceName: alert.device_id || '未知设备',
        probPercent: (prob * 100).toFixed(0),
        severityClass: prob >= 0.85 ? 'severe' : (prob >= 0.70 ? 'warning' : 'normal'),
        acknowledged: Number(alert.acknowledged) === 1,
        cardClass: Number(alert.acknowledged) === 1 ? '' : 'unread',
        statusClass: Number(alert.acknowledged) === 1 ? 'done' : 'new',
        statusText: Number(alert.acknowledged) === 1 ? '已读' : '未读',
        tempText: (alert.temperature_c != null && alert.temperature_c != -999) ? (Number(alert.temperature_c).toFixed(1) + '°C') : '',
        humText: (alert.humidity_rh != null && alert.humidity_rh != -999) ? (Number(alert.humidity_rh).toFixed(1) + '%') : '',
        envText: ((alert.temperature_c != null && alert.temperature_c != -999) ? (Number(alert.temperature_c).toFixed(1) + '°C / ' + Number(alert.humidity_rh).toFixed(1) + '%') : ''),
        _checked: false
      };
    });
  },

  getFallbackStats(alerts, data) {
    const todayStart = new Date().setHours(0, 0, 0, 0);
    return {
      total: Number(data.total) || alerts.length,
      unread: alerts.filter(item => !item.acknowledged).length,
      todayCount: alerts.filter(item => item.ts_ms >= todayStart).length
    };
  }
});
