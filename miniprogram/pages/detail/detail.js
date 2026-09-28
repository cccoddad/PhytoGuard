/*
 * 告警详情页。
 *
 * 展示单条告警的图片、概率、状态与环境信息，
 * 同时提供“标记已读”和图片预览能力。
 */
const api = require('../../utils/api');

Page({
  data: {
    alert: null,
    loading: true
  },

  onLoad(options) {
    const id = options.id;
    if (!id) {
      wx.showToast({ title: '参数错误', icon: 'none' });
      wx.navigateBack();
      return;
    }
    this.loadDetail(id);
  },

  // ---- Swipe right to go back ----
  onTouchStart(e) {
    this._touchStartX = e.touches[0].pageX;
    this._touchStartY = e.touches[0].pageY;
  },

  onTouchEnd(e) {
    const dx = e.changedTouches[0].pageX - (this._touchStartX || 0);
    const dy = Math.abs(e.changedTouches[0].pageY - (this._touchStartY || 0));
    if (dx > 80 && dx > dy * 1.5) {
      wx.navigateBack();
    }
  },

  loadDetail(id) {
    // 详情页拿到的数据会在本地再做一次字段整理，保持与列表页口径一致。
    api.getAlertById(id).then(alert => {
      if (alert.image_url) {
        alert.image_url = api.buildUrl(alert.image_url);
      }
      this.prepareAlert(alert);
      this.setData({ alert, loading: false });
    }).catch(err => {
      console.error('load detail failed:', err);
      this.setData({ loading: false });
      wx.showToast({ title: '加载失败', icon: 'none' });
    });
  },

  markRead() {
    const id = this.data.alert.id;
    api.acknowledgeAlert(id).then(() => {
      this.setData({ 'alert.acknowledged': 1 });
      wx.showToast({ title: '已标记已读', icon: 'success' });
    }).catch(err => {
      wx.showToast({ title: '操作失败', icon: 'none' });
    });
  },

  previewImage() {
    const alert = this.data.alert;
    if (!alert || !alert.image_url) return;
    const imgUrl = api.buildUrl(alert.image_url);
    wx.previewImage({ urls: [imgUrl], current: imgUrl });
  },

  formatTime(ts) {
    const d = new Date(ts);
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  },

  severityLevel(prob) {
    if (prob >= 0.85) return '严重';
    if (prob >= 0.70) return '注意';
    return '轻微';
  },

  prepareAlert(alert) {
    // 复用列表页的展示思路，生成适合详情页直接渲染的字段。
    const prob = Number(alert.top1_prob) || 0;
    alert.displayName = (alert.crop_name && alert.disease_name) ? (alert.crop_name + alert.disease_name) : (alert.top1_label_zh || alert.top1_label || '未知病害');
    alert.cropName = alert.crop_name || '未知作物';
    alert.probPercent = (prob * 100).toFixed(1);
    alert.severityClass = prob >= 0.85 ? 'severe' : (prob >= 0.70 ? 'warning' : 'normal');
    alert.severityText = this.severityLevel(prob);
    alert.acknowledged = Number(alert.acknowledged) === 1;
    alert.statusClass = alert.acknowledged ? 'done' : 'new';
    alert.statusText = alert.acknowledged ? '已读' : '未读';
    alert.tempText = (alert.temperature_c != null && alert.temperature_c != -999) ? (Number(alert.temperature_c).toFixed(1) + '°C') : '';
    alert.humText = (alert.humidity_rh != null && alert.humidity_rh != -999) ? (Number(alert.humidity_rh).toFixed(1) + '%') : '';
  }
});
