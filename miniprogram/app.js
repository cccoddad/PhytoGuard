/*
 * 小程序全局入口。
 *
 * globalData 中保存了后端基础地址、API Key 以及页面间临时共享状态。
 */
App({
  onLaunch() {
    console.log('PhytoGuard mini-program launched');
  },

  globalData: {
    apiBase: 'https://api.plantguard.cn',
    apiKey: 'REPLACE_ME_API_KEY',
    pendingDeviceFilter: ''
  }
});
