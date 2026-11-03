(function () {
  'use strict';

  // R503 LED values: colors 1-7, sequences 1=breathing 2=flashing 3=on 4=off
  var COLORS = [null,
    { n: 'Red', h: '#ef4444' }, { n: 'Blue', h: '#3b82f6' }, { n: 'Purple', h: '#a855f7' },
    { n: 'Green', h: '#22c55e' }, { n: 'Yellow', h: '#eab308' }, { n: 'Cyan', h: '#06b6d4' }, { n: 'White', h: '#f8fafc' }];
  var SEQS = [{ v: 4, n: 'Off' }, { v: 3, n: 'On' }, { v: 1, n: 'Breathing' }, { v: 2, n: 'Blinking' }];

  var $ = function (id) { return document.getElementById(id); };
  var st = null;        // /api/status
  var cfg = null;       // /api/settings
  var fingers = [];     // /api/fingerprints
  var logLines = [];    // oldest first
  var enroll = null;    // running enrollment {iterations, logBefore}

  // ---------- helpers ----------
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function getJson(url) {
    return fetch(url, { cache: 'no-store' }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    });
  }

  // actions of the device answer with a redirect to the web interface, we only care that the request was sent
  function action(url) {
    return fetch(url, { cache: 'no-store' }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
    });
  }

  var toastTimer;
  function toast(msg, bad) {
    var t = $('toast');
    t.textContent = msg;
    t.className = 'show' + (bad ? ' bad' : '');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = ''; }, 2400);
  }

  function fail(e) { toast('Request failed: ' + e.message, true); }

  function fmtUptime(s) {
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    if (d) return d + 'd ' + h + 'h';
    if (h) return h + 'h ' + m + 'm';
    return m + 'm ' + (s % 60) + 's';
  }

  function fmtBytes(b) { return Math.round(b / 1024) + ' KB'; }

  function confirmDialog(title, text, okLabel, cb) {
    $('cfTitle').textContent = title;
    $('cfText').textContent = text;
    $('btnConfirmOk').textContent = okLabel;
    $('btnConfirmOk').onclick = function () { $('dlgConfirm').close(); cb(); };
    $('dlgConfirm').showModal();
  }

  // ---------- routing ----------
  var VIEWS = ['overview', 'fingerprints', 'led', 'connectivity', 'system'];
  function route() {
    var v = location.hash.replace('#', '');
    if (v === 'settings') v = 'connectivity';
    if (VIEWS.indexOf(v) < 0) v = 'overview';
    VIEWS.forEach(function (name) { $('view-' + name).hidden = (name !== v); });
    Array.prototype.forEach.call(document.querySelectorAll('#nav a'), function (a) {
      a.classList.toggle('active', a.getAttribute('data-view') === v);
    });
    updateSaveBar(v);
    window.scrollTo(0, 0);
  }

  // ---------- LED ring preview ----------
  function paintDisc(disc, colorIdx, seq) {
    var c = COLORS[colorIdx] || COLORS[7];
    disc.style.setProperty('--c', c.h);
    disc.setAttribute('data-seq', seq || 3);
  }

  // ---------- rendering: status ----------
  function renderStatus() {
    if (!st) return;
    document.title = st.hostname + ' - FingerprintDoorbell';
    $('brandName').textContent = st.hostname;
    $('brandVersion').textContent = 'Firmware ' + st.version;
    $('sysVersion').textContent = 'Version ' + st.version;

    var title, cls, busy = false;
    if (!st.sensorConnected) { title = 'Sensor not found'; cls = 'state-bad'; }
    else if (st.mode === 'enroll') { title = 'Enrolling'; cls = 'state-warn'; busy = true; }
    else if (st.mode === 'maintenance') { title = 'Busy'; cls = 'state-warn'; busy = true; }
    else if (!st.pairingValid) { title = 'Pairing invalid'; cls = 'state-bad'; }
    else if (st.ignoreTouch) { title = 'Touch ignored'; cls = 'state-warn'; }
    else { title = 'Ready'; cls = 'state-ok'; }
    var h = $('heroTitle');
    h.textContent = title;
    h.className = cls;

    var e = st.lastEvent, sub = 'Waiting for a finger.';
    if (e && e.type === 'match') sub = 'Last: ' + (e.name || 'finger #' + e.id) + ' recognized (confidence ' + e.confidence + ')' + (e.time ? ' - ' + e.time : '');
    else if (e && e.type === 'ring') sub = 'Last: unknown finger rang the bell' + (e.time ? ' - ' + e.time : '');
    if (!st.sensorConnected) sub = 'Check the wiring of the fingerprint sensor.';
    else if (!st.pairingValid) sub = 'Matches are not sent until the sensor is paired again (System).';
    $('heroSub').textContent = sub;

    var disc = $('heroDisc');
    disc.classList.toggle('busy', busy);
    if (!st.ringEnabled) paintDisc(disc, 1, 3);      // gong disabled: on + red
    else if (st.ignoreTouch) paintDisc(disc, 6, 3);  // ignore touch ring: on + cyan
    else if (cfg) paintDisc(disc, cfg.touchRingActiveColor, cfg.touchRingActiveSequence);

    var chips = $('chips');
    chips.textContent = '';
    function chip(label, level) {
      var c = el('span', 'chip');
      c.appendChild(el('span', 'dot ' + level));
      c.appendChild(document.createTextNode(label));
      chips.appendChild(c);
    }
    chip(st.sensorConnected ? 'Sensor connected' : 'Sensor missing', st.sensorConnected ? 'ok' : 'bad');
    chip(st.mqtt === 'connected' ? 'MQTT connected' : st.mqtt === 'unconfigured' ? 'MQTT not configured' : 'MQTT offline',
      st.mqtt === 'connected' ? 'ok' : st.mqtt === 'unconfigured' ? 'warn' : 'bad');
    if (st.mqtt !== 'unconfigured') chip(st.haDiscovery ? 'Home Assistant' : 'Discovery off', st.haDiscovery ? 'ok' : '');
    chip(st.pairingValid ? 'Paired' : 'Not paired', st.pairingValid ? 'ok' : 'bad');

    $('swIgnore').checked = st.ignoreTouch;
    $('swRing').checked = st.ringEnabled;
    $('ringEnabled').checked = st.ringEnabled;

    $('tFingers').innerHTML = '';
    $('tFingers').appendChild(document.createTextNode(String(st.fingerCount)));
    var s = el('small', '', ' / 200');
    $('tFingers').appendChild(s);
    $('tUptime').textContent = fmtUptime(st.uptime);
    $('tIp').textContent = st.ip;
    $('tHeap').textContent = fmtBytes(st.heap);
    $('tRssi').textContent = '';
    $('tRssi').appendChild(document.createTextNode(st.rssi + ' dBm'));
    var level = st.rssi > -55 ? 4 : st.rssi > -65 ? 3 : st.rssi > -75 ? 2 : 1;
    Array.prototype.forEach.call($('tBars').children, function (b, i) { b.className = i < level ? 'on' : ''; });
    $('tEvent').textContent = (e && e.type === 'match') ? (e.name || '#' + e.id) : (e && e.type === 'ring') ? 'Ring' : '-';

    $('sysPairing').textContent = st.pairingValid ? 'Paired with this sensor.' : 'Pairing invalid. Pair the sensor again.';

    if (st.log) setLog(st.log);
    $('liveDot').className = 'dot ok';
    $('liveText').textContent = 'live';
  }

  function setLog(lines) {
    logLines = lines;
    var box = $('log');
    box.textContent = '';
    var shown = lines.filter(function (l) { return l; }).reverse();
    if (!shown.length) { box.appendChild(el('div', 'empty', 'No messages yet.')); return; }
    shown.forEach(function (line) {
      var row = el('div');
      var m = /^\[(.*?)\]:\s?(.*)$/.exec(line);
      if (m) { row.appendChild(el('span', 'ts', m[1])); row.appendChild(document.createTextNode(m[2])); }
      else row.textContent = line;
      box.appendChild(row);
    });
    if (enroll) updateEnroll();
  }

  // ---------- rendering: fingerprints ----------
  function renderFingers() {
    var grid = $('fingerGrid');
    grid.textContent = '';
    $('fpSub').textContent = fingers.length + ' of 200 slots used.';
    if (!fingers.length) {
      var empty = el('div', 'card empty-state');
      var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
      svg.setAttribute('viewBox', '0 0 24 24');
      var use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
      use.setAttribute('href', '#i-fp');
      svg.appendChild(use);
      empty.appendChild(svg);
      empty.appendChild(el('div', '', 'No fingerprints stored yet.'));
      grid.appendChild(empty);
      grid.style.display = 'block';
      return;
    }
    grid.style.display = '';
    fingers.forEach(function (f) {
      var card = el('div', 'finger');
      var top = el('div', 'finger-top');
      top.appendChild(el('div', 'slot', '#' + f.id));
      top.appendChild(el('div', 'finger-name' + (f.name ? '' : ' unnamed'), f.name || 'unnamed'));
      var acts = el('div', 'finger-actions');
      var rn = el('button', 'btn ghost sm', 'Rename');
      rn.onclick = function () { openRename(f); };
      var del = el('button', 'btn danger ghost sm', 'Delete');
      del.onclick = function () {
        confirmDialog('Delete fingerprint #' + f.id + '?', (f.name || 'This fingerprint') + ' will be removed from the sensor.', 'Delete', function () {
          action('/editFingerprints?selectedFingerprint=' + f.id + '&btnDelete=1').then(function () { toast('Deleted'); return loadFingers(); }).catch(fail);
        });
      };
      acts.appendChild(rn);
      acts.appendChild(del);
      card.appendChild(top);
      card.appendChild(acts);
      grid.appendChild(card);
    });
  }

  function openRename(f) {
    $('rnSlot').textContent = 'Slot #' + f.id;
    $('rnName').value = f.name || '';
    $('btnRenameOk').onclick = function () {
      var name = $('rnName').value.trim();
      action('/editFingerprints?selectedFingerprint=' + f.id + '&btnRename=1&renameNewName=' + encodeURIComponent(name))
        .then(function () { $('dlgRename').close(); toast('Saved'); return loadFingers(); }).catch(fail);
    };
    $('dlgRename').showModal();
    $('rnName').focus();
  }

  // ---------- enrollment ----------
  var enIterations = 5;
  function buildIterSeg() {
    var seg = $('enIter');
    seg.textContent = '';
    for (var i = 1; i <= 5; i++) (function (n) {
      var b = el('button', n === enIterations ? 'on' : '', String(n));
      b.type = 'button';
      b.onclick = function () { enIterations = n; buildIterSeg(); };
      seg.appendChild(b);
    })(i);
  }

  function openEnroll() {
    var used = {};
    fingers.forEach(function (f) { used[f.id] = true; });
    var free = 1;
    while (used[free] && free < 200) free++;
    $('enSlot').value = free;
    $('enName').value = '';
    buildIterSeg();
    $('enrollForm').hidden = false;
    $('enrollProgress').hidden = true;
    $('dlgEnroll').showModal();
  }

  function startEnroll() {
    var slot = parseInt($('enSlot').value, 10);
    if (!(slot >= 1 && slot <= 200)) { toast('Slot must be between 1 and 200', true); return; }
    if (fingers.some(function (f) { return f.id === slot; }) && !window.confirm('Slot ' + slot + ' is already used. Replace this fingerprint?')) return;
    enroll = { iterations: enIterations, logBefore: logLines.slice(), started: Date.now() };
    $('enrollForm').hidden = true;
    $('enrollProgress').hidden = false;
    $('btnEnrollDone').hidden = true;
    $('enDisc').classList.add('busy');
    $('enTitle').textContent = 'Place your finger';
    $('enText').textContent = 'Starting enrollment...';
    renderSteps(0);
    var q = '/enroll?startEnrollment=1&newFingerprintId=' + slot + '&newFingerprintName=' + encodeURIComponent($('enName').value.trim()) + '&newFingerprintIteration=' + enIterations;
    action(q).catch(function (e) { enroll = null; $('dlgEnroll').close(); fail(e); });
  }

  function renderSteps(done) {
    var box = $('enSteps');
    box.textContent = '';
    for (var i = 1; i <= enroll.iterations; i++) box.appendChild(el('i', i <= done ? 'done' : i === done + 1 ? 'now' : ''));
  }

  function updateEnroll() {
    if (!enroll) return;
    var fresh = logLines.filter(function (l) { return l && enroll.logBefore.indexOf(l) < 0; });
    var last = fresh.length ? fresh[fresh.length - 1] : '';
    var text = last.replace(/^\[.*?\]:\s?/, '');
    var take = 0;
    fresh.forEach(function (l) { var m = /Take #(\d+)/.exec(l); if (m) take = Math.max(take, parseInt(m[1], 10)); });
    if (text) $('enText').textContent = text;
    if (take) renderSteps(Math.max(0, take - 1));
    var ok = fresh.some(function (l) { return l.indexOf('Enrollment successfull') >= 0; });
    var bad = fresh.some(function (l) { return l.indexOf('Enrollment failed') >= 0 || l.indexOf('Invalid') >= 0; });
    if (ok || bad) {
      enroll = null;
      $('enDisc').classList.remove('busy');
      $('enTitle').textContent = ok ? 'Fingerprint saved' : 'Enrollment failed';
      $('enText').textContent = text;
      if (ok) renderStepsAllDone();
      $('btnEnrollDone').hidden = false;
      loadFingers();
    }
  }

  function renderStepsAllDone() {
    var box = $('enSteps');
    Array.prototype.forEach.call(box.children, function (i) { i.className = 'done'; });
  }

  // ---------- rendering: LED ----------
  function buildLed() {
    Array.prototype.forEach.call(document.querySelectorAll('.swatches'), function (box) {
      var key = box.getAttribute('data-led'), api = box.getAttribute('data-api');
      for (var i = 1; i <= 7; i++) (function (idx) {
        var b = el('button', 'swatch' + (idx > 3 ? ' legacy' : ''));
        b.type = 'button';
        b.style.setProperty('--c', COLORS[idx].h);
        b.title = COLORS[idx].n + (idx > 3 ? ' (newer sensors only)' : '');
        b.setAttribute('aria-label', COLORS[idx].n);
        b.setAttribute('data-v', idx);
        b.onclick = function () { setLed(api, idx); };
        box.appendChild(b);
      })(i);
    });
    var seg = $('segSeq');
    SEQS.forEach(function (s) {
      var b = el('button', '', s.n);
      b.type = 'button';
      b.setAttribute('data-v', s.v);
      b.onclick = function () { setLed('touchRingSequence', s.v); };
      seg.appendChild(b);
    });
  }

  function setLed(setting, value) {
    getJson('/api/led?setting=' + setting + '&value=' + value).then(function (c) { cfg = c; renderLed(); renderStatus(); toast('Saved'); }).catch(fail);
  }

  function renderLed() {
    if (!cfg) return;
    Array.prototype.forEach.call(document.querySelectorAll('.swatches'), function (box) {
      var cur = cfg[box.getAttribute('data-led')];
      Array.prototype.forEach.call(box.children, function (b) { b.classList.toggle('on', parseInt(b.getAttribute('data-v'), 10) === cur); });
    });
    Array.prototype.forEach.call($('segSeq').children, function (b) { b.classList.toggle('on', parseInt(b.getAttribute('data-v'), 10) === cfg.touchRingActiveSequence); });
    paintDisc($('prevReady'), cfg.touchRingActiveColor, cfg.touchRingActiveSequence);
    paintDisc($('prevScan'), cfg.scanColor, 1);
    paintDisc($('prevMatch'), cfg.matchColor, 3);
    if (document.activeElement !== $('ringDuration')) $('ringDuration').value = cfg.ringDuration;
  }

  // ---------- connectivity form ----------
  var FIELDS = [['mqttServer', 'v'], ['mqttUsername', 'v'], ['mqttPassword', 'v'], ['mqttRootTopic', 'v'], ['ntpServer', 'v'], ['haDiscovery', 'c'], ['haDiscoveryPrefix', 'v']];

  function fillForm() {
    FIELDS.forEach(function (f) {
      var input = $(f[0]);
      if (f[1] === 'c') input.checked = !!cfg[f[0]]; else input.value = cfg[f[0]];
    });
    updateTopicHint();
    updateSaveBar();
  }

  function formDirty() {
    if (!cfg) return false;
    return FIELDS.some(function (f) {
      var input = $(f[0]);
      return (f[1] === 'c' ? input.checked : input.value) !== cfg[f[0]];
    });
  }

  function updateSaveBar(view) {
    view = view || location.hash.replace('#', '');
    if (view === 'settings') view = 'connectivity';
    $('savebar').hidden = !(view === 'connectivity' && formDirty());
  }

  function updateTopicHint() {
    var r = $('mqttRootTopic').value || '...';
    $('topicHint').textContent = 'Topics: ' + r + '/ring, matchName, matchId, matchConfidence, connection, uptime, ... (commands: ' + r + '/ignoreTouchRing, ringEnabled, reboot)';
  }

  function saveAll() {
    if (!$('mqttServer').value.trim() || !$('mqttRootTopic').value.trim()) { toast('Broker and root topic are required', true); return; }
    var p = new URLSearchParams();
    p.set('btnSaveSettings', '1');
    p.set('mqtt_server', $('mqttServer').value.trim());
    p.set('mqtt_username', $('mqttUsername').value);
    p.set('mqtt_password', $('mqttPassword').value);
    p.set('mqtt_rootTopic', $('mqttRootTopic').value.trim());
    p.set('ntpServer', $('ntpServer').value.trim());
    if ($('haDiscovery').checked) p.set('haDiscovery', '1');
    p.set('haDiscoveryPrefix', $('haDiscoveryPrefix').value.trim());
    p.set('touchRingActiveColor', cfg.touchRingActiveColor);
    p.set('touchRingActiveSequence', cfg.touchRingActiveSequence);
    p.set('scanColor', cfg.scanColor);
    p.set('matchColor', cfg.matchColor);
    action('/settings?' + p.toString()).then(function () { restartOverlay('Saving and restarting...'); }).catch(fail);
  }

  // ---------- restart handling ----------
  function restartOverlay(title, noReload) {
    $('ovTitle').textContent = title;
    $('overlay').hidden = false;
    if (noReload) { $('ovText').textContent = 'Connect to the Wi-Fi "FingerprintDoorbell-Config" to set up the device again.'; return; }
    setTimeout(function wait() {
      fetch('/api/status', { cache: 'no-store' }).then(function (r) { if (r.ok) location.reload(); else setTimeout(wait, 1500); }).catch(function () { setTimeout(wait, 1500); });
    }, 5000);
  }

  // ---------- data loading ----------
  function loadFingers() { return getJson('/api/fingerprints').then(function (f) { fingers = f; renderFingers(); }); }
  function loadStatus() {
    return getJson('/api/status').then(function (s) { st = s; renderStatus(); }).catch(function () {
      $('liveDot').className = 'dot bad';
      $('liveText').textContent = 'offline';
    });
  }

  // ---------- init ----------
  function init() {
    buildLed();
    window.addEventListener('hashchange', route);

    $('swIgnore').onchange = function () {
      getJson('/api/control?ignoreTouchRing=' + (this.checked ? 'on' : 'off')).then(function (s) { st = s; renderStatus(); }).catch(fail);
    };
    $('swRing').onchange = function () {
      getJson('/api/control?ringEnabled=' + (this.checked ? 'on' : 'off')).then(function (s) { st = s; renderStatus(); }).catch(fail);
    };
    $('btnSaveRing').onclick = function () {
      var d = parseInt($('ringDuration').value, 10);
      if (!(d >= 50 && d <= 5000)) { toast('Duration must be 50-5000 ms', true); return; }
      action('/ring?btnSaveRing=1&ringDuration=' + d + ($('ringEnabled').checked ? '&ringEnabled=on' : '')).then(function () {
        toast('Saved');
        return Promise.all([getJson('/api/settings').then(function (c) { cfg = c; renderLed(); }), loadStatus()]);
      }).catch(fail);
    };

    $('btnAdd').onclick = openEnroll;
    $('btnEnrollStart').onclick = startEnroll;
    $('btnEnrollDone').onclick = function () { $('dlgEnroll').close(); };
    Array.prototype.forEach.call(document.querySelectorAll('[data-close]'), function (b) {
      b.onclick = function () { b.closest('dialog').close(); };
    });

    FIELDS.forEach(function (f) {
      var ev = f[1] === 'c' ? 'change' : 'input';
      $(f[0]).addEventListener(ev, function () { updateTopicHint(); updateSaveBar(); });
    });
    $('btnDiscard').onclick = fillForm;
    $('btnSaveAll').onclick = saveAll;
    $('btnShowPw').onclick = function () {
      var pw = $('mqttPassword');
      pw.type = pw.type === 'password' ? 'text' : 'password';
      this.textContent = pw.type === 'password' ? 'Show' : 'Hide';
    };

    $('btnPair').onclick = function () {
      confirmDialog('Pair a new sensor?', 'Use this after replacing the fingerprint sensor. Only do this if you trust the sensor.', 'Pair', function () {
        action('/pairing?btnDoPairing=1').then(function () { toast('Pairing requested'); setTimeout(loadStatus, 800); }).catch(fail);
      });
    };
    $('btnReboot').onclick = function () {
      confirmDialog('Restart device?', 'The device is unavailable for a few seconds.', 'Restart', function () {
        action('/reboot').then(function () { restartOverlay('Restarting...'); }).catch(fail);
      });
    };
    $('btnDeleteAll').onclick = function () {
      confirmDialog('Delete all fingerprints?', 'This removes every fingerprint from the sensor.', 'Delete all', function () {
        action('/deleteAllFingerprints?btnDeleteAllFingerprints=1').then(function () { toast('Deleted'); return loadFingers(); }).catch(fail);
      });
    };
    $('btnFactory').onclick = function () {
      confirmDialog('Factory reset?', 'This deletes all fingerprints, your settings and the Wi-Fi configuration.', 'Reset everything', function () {
        action('/factoryReset?btnFactoryReset=1').then(function () { restartOverlay('Factory reset...', true); }).catch(fail);
      });
    };

    // server sent events: instant log messages and fingerlist updates
    if (window.EventSource) {
      var es = new EventSource('/events');
      es.addEventListener('message', function (e) { setLog(e.data.split('<br>').filter(function (l) { return l; })); });
      es.addEventListener('fingerlist', function (e) { try { fingers = JSON.parse(e.data); renderFingers(); } catch (x) { /* ignore */ } });
    }

    Promise.all([getJson('/api/settings'), getJson('/api/fingerprints'), getJson('/api/status')]).then(function (r) {
      cfg = r[0]; fingers = r[1]; st = r[2];
      fillForm(); renderLed(); renderFingers(); renderStatus(); route();
    }).catch(function () { route(); fail(new Error('device not reachable')); });

    setInterval(function () { if (!document.hidden) loadStatus(); }, 4000);
  }

  init();
})();
