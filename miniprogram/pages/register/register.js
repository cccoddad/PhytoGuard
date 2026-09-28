/*
 * 设备注册页。
 *
 * 手动录入 device_id / 名称 / 位置，调用 POST /api/devices 注册。
 * 注册成功后返回设备页并刷新列表。
 */
const api = require('../../utils/api');

Page({
  data: {
    deviceId: '',
    name: '',
    location: '',
    submitting: false,
    error: ''
  },

  onInput(e) {
    const field = e.currentTarget.dataset.field;
    this.setData({ [field]: e.detail.value, error: '' });
  },

  onSubmit() {
    const deviceId = (this.data.deviceId || '').trim();
    if (!deviceId) {
      this.setData({ error: '请填写设备 ID' });
        return;
    }
    if (this.data.submitting) return;

    this.setData({ submitting: true, error: '' });
    api.registerDevice(deviceId, (this.data.name || '').trim(), (this.data.location || '').trim())
      .then(() => {
        // 标记设备页需要刷新，返回后能立即看到新设备。
        const pages = getCurrentPages();
        const prev = pages[pages.length - 2];
        if (prev) prev._needsRefresh = true;
        wx.navigateBack();
      })
      .catch(err => {
        this.setData({
          submitting: false,
          error: (err && err.msg) || '注册失败，请检查网络'
        });
      });
  }
});
