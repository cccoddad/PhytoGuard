/*
 * 小程序端统一 API 封装。
 *
 * 目标：
 * 1. 统一拼接服务端地址。
 * 2. 统一注入 X-API-Key。
 * 3. 统一把微信请求错误转成 Promise reject，简化页面层逻辑。
 */
const app = getApp();

function getBaseUrl() {
  return (app.globalData.apiBase || '').replace(/\/$/, '');
}

function getHeaders() {
  const headers = { 'Content-Type': 'application/json' };
  if (app.globalData.apiKey) {
    headers['X-API-Key'] = app.globalData.apiKey;
  }
  return headers;
}

function isAbsoluteUrl(url) {
  return /^https?:\/\//i.test(url || '');
}

function buildUrl(path) {
  if (!path) return '';
  if (isAbsoluteUrl(path)) return path;
  return getBaseUrl() + (path.charAt(0) === '/' ? path : '/' + path);
}

function getErrorMessage(data) {
  if (!data) return '';
  if (typeof data === 'string') return data;
  return data.error || data.message || data.msg || '';
}

function request(method, path, data) {
  // 页面层只需要关心业务参数，不需要重复写 wx.request 样板代码。
  return new Promise((resolve, reject) => {
    wx.request({
      url: buildUrl(path),
      method: method,
      data: data,
      header: getHeaders(),
      success(res) {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          resolve(res.data);
        } else {
          reject({
            code: res.statusCode,
            msg: getErrorMessage(res.data) || '请求失败'
          });
        }
      },
      fail(err) {
        reject({ code: -1, msg: err.errMsg });
      }
    });
  });
}

// Alert APIs
function getAlerts(page = 1, limit = 20, deviceId = '') {
  let path = `/api/alerts?page=${page}&limit=${limit}`;
  if (deviceId) {
    path += `&device_id=${encodeURIComponent(deviceId)}`;
  }
  return request('GET', path);
}

function getStats(deviceId = '') {
  let path = '/api/stats';
  if (deviceId) {
    path += `?device_id=${encodeURIComponent(deviceId)}`;
  }
  return request('GET', path);
}

function getAlertById(id) {
  return request('GET', `/api/alerts/${encodeURIComponent(id)}`);
}

function acknowledgeAlert(id) {
  return request('PATCH', `/api/alerts/${encodeURIComponent(id)}/ack`);
}

function deleteAlert(id) {
  return request('DELETE', `/api/alerts/${encodeURIComponent(id)}`);
}

// Device APIs
function getDevices() {
  return request('GET', '/api/devices');
}

function registerDevice(deviceId, name, location) {
  return request('POST', '/api/devices', {
    device_id: deviceId,
    name: name || '',
    location: location || ''
  });
}

module.exports = {
  buildUrl,
  getAlerts,
  getStats,
  getAlertById,
  acknowledgeAlert,
  deleteAlert,
  getDevices,
  registerDevice
};
