// ==UserScript==
// @name         音游伴侣 - 导出谱子数据
// @namespace    zmk-3x5-bt
// @version      0.1
// @description  拦截曲谱音符数据，导出为 JSON
// @match        https://mgm.jie-you.cn/scores/*
// @grant        none
// @run-at       document-idle
// ==/UserScript==

(function () {
  'use strict';

  let capturedData = null;
  let btn = null;

  // Hook fetch
  const originalFetch = window.fetch;
  window.fetch = async function (...args) {
    const response = await originalFetch.apply(this, args);
    try {
      const clone = response.clone();
      const text = await clone.text();
      // 尝试解析为 JSON
      const json = JSON.parse(text);
      // 识别音符数组：response.data 里有 notes/notes/track 字段，或顶层就是数组
      const noteArr = 
        json?.data?.notes ||
        json?.data?.noteList ||
        json?.data?.tracks?.[0]?.notes ||
        json?.data?.tracks?.[0]?.noteList ||
        (Array.isArray(json?.data) ? json.data : null) ||
        (Array.isArray(json?.notes) ? json.notes : null) ||
        (Array.isArray(json?.data?.data) ? json.data.data : null);

      if (noteArr && Array.isArray(noteArr) && noteArr.length > 0) {
        // 检查是否有 time + key/keyCode/note 字段（音符特征）
        const sample = noteArr[0];
        if (sample && (sample.time !== undefined || sample.t !== undefined || sample.start !== undefined)) {
          capturedData = noteArr;
          showButton();
        }
      }
    } catch (_) {}
    return response;
  };

  // Hook XHR
  const originalXHROpen = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    this._url = url;
    return originalXHROpen.call(this, method, url, ...rest);
  };
  const originalXHRSend = XMLHttpRequest.prototype.send;
  XMLHttpRequest.prototype.send = function (...args) {
    this.addEventListener('load', function () {
      try {
        const json = JSON.parse(this.responseText);
        const noteArr =
          json?.data?.notes ||
          json?.data?.noteList ||
          json?.data?.tracks?.[0]?.notes ||
          json?.data?.tracks?.[0]?.noteList ||
          (Array.isArray(json?.data) ? json.data : null) ||
          (Array.isArray(json?.notes) ? json.notes : null);

        if (noteArr && Array.isArray(noteArr) && noteArr.length > 0) {
          const sample = noteArr[0];
          if (sample && (sample.time !== undefined || sample.t !== undefined || sample.start !== undefined)) {
            capturedData = noteArr;
            showButton();
          }
        }
      } catch (_) {}
    });
    return originalXHRSend.apply(this, args);
  };

  function showButton() {
    if (btn) return;
    btn = document.createElement('div');
    btn.style.cssText = `
      position: fixed; top: 80px; right: 20px; z-index: 99999;
      background: #ff6b35; color: white; padding: 12px 20px;
      border-radius: 8px; cursor: pointer; font-size: 14px;
      font-family: sans-serif; box-shadow: 0 4px 12px rgba(0,0,0,0.3);
    `;
    btn.textContent = '⬇ 导出谱子 JSON';
    btn.onclick = downloadJSON;
    document.body.appendChild(btn);
  }

  function downloadJSON() {
    if (!capturedData) return;
    const blob = new Blob([JSON.stringify(capturedData, null, 2)], {type: 'application/json'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'score_raw_' + Date.now() + '.json';
    a.click();
    URL.revokeObjectURL(url);
  }
})();
