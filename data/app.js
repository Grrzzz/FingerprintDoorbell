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
  // ---------- release check (the browser asks GitHub, the device itself needs no internet access) ----------
  var RELEASES_API = 'https://api.github.com/repos/Grrzzz/FingerprintDoorbell/releases/latest';
  var updateChecked = false;

  function versionParts(v) {
    var m = /(\d+)(?:\.(\d+))?(?:\.(\d+))?/.exec(v || '');
    return m ? [parseInt(m[1], 10), parseInt(m[2] || '0', 10), parseInt(m[3] || '0', 10)] : [0, 0, 0];
  }

  function isNewer(a, b) {
    var x = versionParts(a), y = versionParts(b);
    for (var i = 0; i < 3; i++) if (x[i] !== y[i]) return x[i] > y[i];
    return false;
  }

  // one-click update: the device downloads firmware and web interface from the GitHub release itself
  function startSelfUpdate(tag) {
    confirmDialog('Update to ' + tag + '?', 'The device downloads the new web interface and firmware from GitHub and restarts. This takes a few minutes. Do not unplug it meanwhile.', 'Update', function () {
      fetch('/api/selfupdate/start?tag=' + encodeURIComponent(tag), { cache: 'no-store' }).then(function (r) {
        return r.text().then(function (t) { if (!r.ok) throw new Error(t || 'HTTP ' + r.status); });
      }).then(function () { watchSelfUpdate(tag); }).catch(function (e) { toast('Update not started: ' + e.message, true); });
    });
  }

  function watchSelfUpdate(tag) {
    $('ovTitle').textContent = 'Updating to ' + tag;
    $('ovText').textContent = 'Starting...';
    $('ovBar').style.width = '0%';
    $('ovBarWrap').hidden = false;
    $('overlay').hidden = false;
    var failures = 0;
    (function poll() {
      getJson('/api/status').then(function (s) {
        failures = 0;
        var u = s.update || {};
        if (u.state === 'error') {
          $('overlay').hidden = true;
          $('ovBarWrap').hidden = true;
          toast('Update failed: ' + u.message, true);
          return;
        }
        if (u.state === 'restarting') {
          $('ovBarWrap').hidden = true;
          restartOverlay('Restarting...');
          return;
        }
        $('ovText').textContent = (u.message || 'Working') + ' (' + u.percent + ' %)';
        $('ovBar').style.width = u.percent + '%';
        setTimeout(poll, 1000);
      }).catch(function () {
        failures++;
        if (failures > 6) { $('ovBarWrap').hidden = true; restartOverlay('Restarting...'); return; }
        setTimeout(poll, 1500);
      });
    })();
  }

  // ---------- storage chart ----------
  // the flash area is organized in 32 byte entries, show plain kilobytes
  function kb(entries) { return (entries * 32 / 1024).toFixed(1) + ' KB'; }

  function loadStorage() {
    getJson('/api/storage').then(function (d) {
      if (!d.total) return;
      var usable = d.total - d.reserved;
      var free = Math.max(0, d.total - d.reserved - d.fingers - d.settings - d.other);
      var parts = [
        { cls: 'st-fingers', label: 'Fingerprints', n: d.fingers },
        { cls: 'st-settings', label: 'Settings', n: d.settings },
        { cls: 'st-other', label: 'System and other', n: d.other },
        { cls: 'st-free', label: 'Free', n: free },
        { cls: 'st-reserved', label: 'Reserved', n: d.reserved }
      ];
      var bar = $('stStack'), legend = $('stLegend');
      bar.textContent = '';
      legend.textContent = '';
      parts.forEach(function (p) {
        if (p.n > 0) {
          var seg = el('i', p.cls);
          seg.style.width = (p.n * 100 / d.total) + '%';
          seg.title = p.label + ': ' + kb(p.n) + ' of ' + kb(d.total);
          bar.appendChild(seg);
        }
        var item = el('span', 'st-item');
        item.appendChild(el('i', p.cls));
        item.appendChild(document.createTextNode(p.label + ' ' + kb(p.n) + ' (' + Math.round(p.n * 100 / d.total) + ' %)'));
        legend.appendChild(item);
      });
      var perFinger = d.fingerCount > 0 && d.fingers > 0 ? d.fingers / d.fingerCount : 1.5;
      var more = Math.floor(free / Math.max(1, perFinger));
      var pct = Math.round((d.total - d.reserved - free) * 100 / usable);
      var hint = kb(d.total - d.reserved - free) + ' of ' + kb(usable) + ' usable (' + pct + ' %). About ' + more + ' more fingerprint' + (more === 1 ? '' : 's') + ' fit.';
      if (pct >= 85) hint += ' Almost full: delete fingerprints or shorten names.';
      $('stHint').textContent = hint;
      $('stHint').style.color = pct >= 85 ? 'var(--bad)' : '';
    }).catch(function () {});
  }

  function checkUpdate(manual) {
    var box = $('sysUpdate');
    if (!st || (updateChecked && !manual)) return;
    updateChecked = true;
    box.className = 'd';
    box.textContent = 'Checking for updates...';
    fetch(RELEASES_API, { headers: { Accept: 'application/vnd.github+json' } }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (rel) {
      box.textContent = '';
      if (isNewer(rel.tag_name, st.version)) {
        box.className = 'd new';
        box.appendChild(document.createTextNode('Update available: ' + (rel.name || rel.tag_name) + ' - '));
        var a = el('a', '', 'view release');
        a.href = rel.html_url; a.target = '_blank'; a.rel = 'noopener';
        box.appendChild(a);
        var go = el('button', 'btn sm', 'Update now');
        go.style.marginLeft = '10px';
        go.onclick = function () { startSelfUpdate(rel.tag_name); };
        box.appendChild(go);
      } else {
        box.textContent = 'You are up to date (latest release: ' + rel.tag_name + ').';
      }
    }).catch(function () {
      updateChecked = false;
      box.textContent = 'Could not check for updates (no internet connection in this browser?).';
    });
  }

  function route() {
    var v = location.hash.replace('#', '');
    if (v === 'settings') v = 'connectivity';
    if (VIEWS.indexOf(v) < 0) v = 'overview';
    VIEWS.forEach(function (name) { $('view-' + name).hidden = (name !== v); });
    Array.prototype.forEach.call(document.querySelectorAll('#nav a'), function (a) {
      a.classList.toggle('active', a.getAttribute('data-view') === v);
    });
    updateSaveBar(v);
    if (v === 'system') { checkUpdate(false); loadStorage(); }
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
    else if (e && e.type === 'disabled') sub = 'Last: disabled fingerprint ' + (e.name || '#' + e.id) + ' detected, treated as unknown' + (e.time ? ' - ' + e.time : '');
    if (!st.sensorConnected) sub = 'Check the wiring of the fingerprint sensor.';
    else if (!st.pairingValid) sub = 'Matches are not sent until the sensor is paired again (System).';
    $('heroSub').textContent = sub;

    var disc = $('heroDisc');
    disc.classList.toggle('busy', busy);
    if (cfg) {
      if (!st.ringEnabled) paintDisc(disc, cfg.gongOffColor, cfg.gongOffSequence);        // gong disabled
      else if (st.ignoreTouch) paintDisc(disc, cfg.ignoreColor, cfg.ignoreSequence);       // ignore touch ring
      else paintDisc(disc, cfg.touchRingActiveColor, cfg.touchRingActiveSequence);
    }

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
    $('tEvent').textContent = (e && e.type === 'match') ? shortName(e.name || '#' + e.id) : (e && e.type === 'ring') ? 'Ring' : (e && e.type === 'disabled') ? 'Disabled finger' : '-';

    $('sysPairing').textContent = st.pairingValid ? 'Paired with this sensor.' : 'Pairing invalid. Pair the sensor again.';

    if (st.log) setLog(st.log);
    Array.prototype.forEach.call(document.querySelectorAll('.wk-now'), positionNow);
    if (location.hash === '#system') checkUpdate(false);
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
  // names like "Christopher (Daumen rechts) #Garage": name bold, finger italic, action as colored badge
  var ACTION_CLASSES = { 'haustür': 'act-door', 'haustuer': 'act-door', 'garage': 'act-garage', 'reinigung': 'act-clean', 'front door': 'act-door', 'cleaning': 'act-clean' };

  // "Name (Finger) #Action" -> parts, otherwise null
  function parseFingerName(name) {
    var m = /^(.+?)\s*\(([^()]+)\)\s*#(.+)$/.exec(name || '');
    if (!m) return null;
    var action = m[3].trim();
    return { name: m[1].trim(), finger: m[2].trim(), action: action, cls: ACTION_CLASSES[action.toLowerCase()] || 'act-other' };
  }

  // structured names ("Name (Finger) #Action") are shortened to the name in small places
  function shortName(name) {
    var p = parseFingerName(name);
    return p ? p.name : name;
  }

  function icon(id) {
    var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    var use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', '#' + id);
    svg.appendChild(use);
    return svg;
  }

  function iconButton(id, label, cls, onclick, withText) {
    var b = el('button', 'btn ghost sm ' + cls);
    b.type = 'button';
    b.title = label;
    b.setAttribute('aria-label', label);
    b.appendChild(icon(id));
    if (withText) b.appendChild(document.createTextNode(label));
    b.onclick = onclick;
    return b;
  }

  // ---------- fingerprint on/off and time window ----------
  var DAY_NAMES = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

  function pad2(n) { return (n < 10 ? '0' : '') + n; }
  function fmtMinutes(m) { return pad2(Math.floor(m / 60)) + ':' + pad2(m % 60); }
  function parseMinutes(v) { var m = /^(\d{1,2}):(\d{2})/.exec(v || ''); return m ? Math.min(1439, parseInt(m[1], 10) * 60 + parseInt(m[2], 10)) : 0; }

  function describeDays(mask) {
    if (mask === 127) return 'Every day';
    if (mask === 31) return 'Mon-Fri';
    if (mask === 96) return 'Sat-Sun';
    var out = [];
    DAY_NAMES.forEach(function (n, i) { if (mask & (1 << i)) out.push(n); });
    return out.length ? out.join(', ') : 'No day selected';
  }

  function describeWindows(f) {
    return (f.windows || []).map(function (w) {
      return describeDays(w.days) + ' ' + (w.allDay ? 'all day' : fmtMinutes(w.from) + '-' + fmtMinutes(w.to));
    });
  }

  function setPolicy(f, params) {
    var q = '/api/finger/policy?id=' + f.id;
    Object.keys(params).forEach(function (k) { q += '&' + k + '=' + encodeURIComponent(params[k]); });
    return getJson(q).then(function (list) { fingers = normalizeFingers(list); renderFingers(); });
  }

  var MAX_WINDOWS = 4;
  var sch = null;            // {f, rules: [{days, from, to, allDay}]}
  var schInit = false;

  // custom hour/minute picker instead of the browser's native time popup
  function closeTimePicker() { $('timePop').hidden = true; }

  function openTimePicker(anchor, cur, onPick) {
    var pop = $('timePop');
    pop.textContent = '';

    function section(title, cls) {
      var wrap = el('div', 'tp-sec');
      wrap.appendChild(el('div', 'tp-title', title));
      var grid = el('div', 'tp-grid ' + cls);
      wrap.appendChild(grid);
      pop.appendChild(wrap);
      return grid;
    }
    function apply(h, m, close) {
      cur = h * 60 + m;
      onPick(cur);
      if (close) closeTimePicker(); else openTimePicker(anchor, cur, onPick);
    }
    var hGrid = section('Hour', 'hours');
    for (var h = 0; h < 24; h++) (function (hh) {
      var b = el('button', hh === Math.floor(cur / 60) ? 'on' : '', pad2(hh));
      b.type = 'button';
      b.onclick = function () { apply(hh, cur % 60, false); };
      hGrid.appendChild(b);
    })(h);
    var mGrid = section('Minute', 'mins');
    var mins = [];
    for (var m = 0; m < 60; m += 5) mins.push(m);
    if (mins.indexOf(cur % 60) < 0) { mins.push(cur % 60); mins.sort(function (x, y) { return x - y; }); }
    mins.forEach(function (mm) {
      var b = el('button', mm === cur % 60 ? 'on' : '', pad2(mm));
      b.type = 'button';
      b.onclick = function () { apply(Math.floor(cur / 60), mm, true); };
      mGrid.appendChild(b);
    });

    // fixed position: the rule list scrolls, the popover must not be clipped by it
    pop.hidden = false;
    var r = anchor.getBoundingClientRect();
    var left = Math.min(Math.max(8, r.left), window.innerWidth - pop.offsetWidth - 8);
    var top = r.bottom + 8;
    if (top + pop.offsetHeight > window.innerHeight - 8) top = Math.max(8, r.top - pop.offsetHeight - 8);
    pop.style.left = left + 'px';
    pop.style.top = top + 'px';
  }

  function durationText(r) {
    if (r.allDay) return '24 h';
    if (r.from === r.to) return 'Invalid';
    var len = r.to > r.from ? r.to - r.from : 1440 - r.from + r.to;
    return Math.floor(len / 60) + ' h' + (len % 60 ? ' ' + (len % 60) + ' min' : '') + (r.to < r.from ? ' · overnight' : '');
  }

  // one row per weekday: where the windows put the fingerprint into "active"
  function renderWeek() { drawWeek($('schWeek'), sch.rules); }

  // red marker at the current device time inside a week overview
  function positionNow(node) {
    if (!st || !st.clock) { node.hidden = true; return; }
    node.hidden = false;
    node.style.left = 'calc(30px + (100% - 30px) * ' + (st.clock.min / 1440) + ')';
    node.firstChild.textContent = fmtMinutes(st.clock.min);
  }

  function drawWeek(box, rules) {
    box.textContent = '';
    box.classList.add('wk-box');
    DAY_NAMES.forEach(function (n, k) {
      var row = el('div', 'wk-row' + (st && st.clock && st.clock.day === k ? ' today' : ''));
      row.appendChild(el('b', '', n.slice(0, 2)));
      var track = el('div', 'wk-track');
      var prev = (k + 6) % 7;
      function seg(a, b) {
        if (b <= a) return;
        var i = el('i');
        i.style.left = (a / 14.4) + '%';
        i.style.width = ((b - a) / 14.4) + '%';
        track.appendChild(i);
      }
      rules.forEach(function (r) {
        var today = r.days & (1 << k), yesterday = r.days & (1 << prev);
        if (r.allDay) { if (today) seg(0, 1440); return; }
        if (r.from === r.to) return;
        if (r.from < r.to) { if (today) seg(r.from, r.to); }
        else { if (today) seg(r.from, 1440); if (yesterday) seg(0, r.to); }
      });
      row.appendChild(track);
      box.appendChild(row);
    });
    var now = el('div', 'wk-now');
    now.appendChild(el('span', ''));
    box.appendChild(now);
    positionNow(now);
  }

  function renderRules() {
    var host = $('schRules');
    host.textContent = '';
    closeTimePicker();
    sch.rules.forEach(function (r, idx) {
      var card = el('div', 'rule');
      var head = el('div', 'rule-head');
      head.appendChild(el('span', 'rule-title', 'Window ' + (idx + 1)));
      var dur = el('span', 'dur-pill');
      function setDur() { dur.textContent = durationText(r); dur.classList.toggle('bad', !r.allDay && r.from === r.to); }
      head.appendChild(dur);
      if (sch.rules.length > 1) {
        var del = el('button', 'icon-only');
        del.type = 'button';
        del.title = 'Remove window';
        del.setAttribute('aria-label', 'Remove window');
        del.appendChild(icon('i-trash'));
        del.onclick = function () { sch.rules.splice(idx, 1); renderRules(); renderWeek(); };
        head.appendChild(del);
      }
      card.appendChild(head);

      var days = el('div', 'day-seg');
      DAY_NAMES.forEach(function (n, k) {
        var b = el('button', r.days & (1 << k) ? 'on' : '', n.slice(0, 2));
        b.type = 'button';
        b.title = n;
        b.onclick = function () { r.days ^= (1 << k); b.classList.toggle('on'); renderWeek(); };
        days.appendChild(b);
      });
      card.appendChild(days);

      var times = el('div', 'rule-times');
      function timeBox(label, key) {
        var box = el('div', 'time-box');
        box.appendChild(el('span', '', label));
        var btn = el('button', 'time-val', fmtMinutes(r[key]));
        btn.type = 'button';
        btn.onclick = function (e) {
          e.stopPropagation();
          openTimePicker(box, r[key], function (v) { r[key] = v; btn.textContent = fmtMinutes(v); setDur(); renderWeek(); });
        };
        box.appendChild(btn);
        return box;
      }
      var from = timeBox('From', 'from'), to = timeBox('To', 'to');
      var arrow = el('span', 'time-arrow', '→');
      var allDay = el('label', 'allday');
      var cb = el('input');
      cb.type = 'checkbox';
      cb.checked = !!r.allDay;
      var track = el('span', 'switch sm');
      track.appendChild(cb);
      track.appendChild(el('span', 'track'));
      allDay.appendChild(track);
      allDay.appendChild(el('span', '', 'All day'));
      function syncAllDay() { from.hidden = to.hidden = arrow.hidden = !!r.allDay; setDur(); }
      cb.onchange = function () { r.allDay = cb.checked; syncAllDay(); renderWeek(); };
      times.appendChild(from);
      times.appendChild(arrow);
      times.appendChild(to);
      times.appendChild(allDay);
      card.appendChild(times);
      syncAllDay();
      host.appendChild(card);
    });
    $('btnAddWindow').hidden = sch.rules.length >= MAX_WINDOWS;
  }

  // hover preview of the weekly schedule on the fingerprint cards
  var weekTip = null;
  function showWeekTip(anchor, f) {
    if (!weekTip) {
      weekTip = el('div', 'week-tip');
      weekTip.hidden = true;
      document.body.appendChild(weekTip);
    }
    weekTip.textContent = '';
    weekTip.appendChild(el('div', 'wt-title', 'Weekly schedule · ' + (f.active === false ? 'inactive now' : 'active now')));
    var grid = el('div');
    weekTip.appendChild(grid);
    drawWeek(grid, f.windows || []);
    var scale = el('div', 'wk-scale');
    ['0', '6', '12', '18', '24'].forEach(function (t) { scale.appendChild(el('span', '', t)); });
    weekTip.appendChild(scale);
    weekTip.hidden = false;
    var r = anchor.getBoundingClientRect();
    var left = Math.min(Math.max(8, r.left), window.innerWidth - weekTip.offsetWidth - 8);
    var top = r.bottom + 8;
    if (top + weekTip.offsetHeight > window.innerHeight - 8) top = Math.max(8, r.top - weekTip.offsetHeight - 8);
    weekTip.style.left = left + 'px';
    weekTip.style.top = top + 'px';
  }
  function hideWeekTip() { if (weekTip) weekTip.hidden = true; }

  function openSchedule(f) {
    hideWeekTip();
    if (!schInit) {
      schInit = true;
      document.addEventListener('mousedown', function (e) {
        var pop = $('timePop');
        if (!pop.hidden && !pop.contains(e.target) && !e.target.classList.contains('time-val')) closeTimePicker();
      });
      $('dlgSchedule').addEventListener('cancel', function (e) {
        if (!$('timePop').hidden) { e.preventDefault(); closeTimePicker(); }
      });
      $('schOn').onchange = function () { $('schBody').classList.toggle('off', !this.checked); };
      $('btnAddWindow').onclick = function () {
        if (sch.rules.length >= MAX_WINDOWS) return;
        sch.rules.push({ days: 31, from: 480, to: 1080, allDay: false });
        renderRules();
        renderWeek();
      };
      $('btnScheduleOk').onclick = function () {
        var on = $('schOn').checked;
        if (on) {
          for (var i = 0; i < sch.rules.length; i++) {
            var r = sch.rules[i];
            if (!r.days) { toast('Window ' + (i + 1) + ': select at least one day', true); return; }
            if (!r.allDay && r.from === r.to) { toast('Window ' + (i + 1) + ': From and To must differ', true); return; }
          }
        }
        var w = sch.rules.map(function (r) { return [r.days, r.from, r.to, r.allDay ? 1 : 0].join(','); }).join(';');
        setPolicy(sch.f, { scheduled: on ? 1 : 0, w: w })
          .then(function () { $('dlgSchedule').close(); toast('Saved'); }).catch(fail);
      };
    }
    sch = { f: f, rules: (f.windows && f.windows.length ? f.windows : [{ days: 31, from: 480, to: 1080, allDay: false }]).map(function (w) {
      return { days: w.days, from: w.from, to: w.to, allDay: !!w.allDay };
    }) };
    $('schSlot').textContent = 'Slot #' + f.id + (f.name ? ' - ' + f.name : '');
    $('schOn').checked = !!f.scheduled;
    $('schBody').classList.toggle('off', !f.scheduled);
    renderRules();
    renderWeek();
    $('dlgSchedule').showModal();
  }

  function renderFingers() {
    hideWeekTip();
    var grid = $('fingerGrid');
    grid.textContent = '';
    var manualOff = fingers.filter(function (f) { return !f.scheduled && f.enabled === false; }).length;
    var plans = fingers.filter(function (f) { return f.scheduled; }).length;
    $('fpSub').textContent = fingers.length + ' of 200 slots used · ' + manualOff + ' manually disabled · ' + plans + ' schedule' + (plans === 1 ? '' : 's') + ' active';
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
    var q = ($('fpSearch').value || '').trim().toLowerCase();
    var list = fingers.filter(function (f) {
      if (!q) return true;
      var state = f.scheduled ? (f.active === false ? 'inactive scheduled' : 'active scheduled') : (f.enabled === false ? 'disabled' : 'active');
      return ('#' + f.id + ' ' + (f.name || 'unnamed') + ' ' + state).toLowerCase().indexOf(q) >= 0;
    });
    if (q) $('fpSub').textContent = list.length + ' of ' + fingers.length + ' fingerprints match.';
    if (!list.length) {
      var none = el('div', 'card empty-state');
      none.appendChild(el('div', '', 'No fingerprint matches "' + $('fpSearch').value.trim() + '".'));
      grid.appendChild(none);
      grid.style.display = 'block';
      return;
    }
    list.forEach(function (f) {
      var info = parseFingerName(f.name);
      // with a schedule the schedule decides: the switch then only shows the current state
      var on = f.scheduled ? f.active !== false : f.enabled !== false;
      var state = f.scheduled ? (on ? 'ok' : 'wait') : (on ? 'ok' : 'off');
      // greyed out like a manually disabled card whenever the fingerprint is currently inactive (switch or schedule)
      var card = el('div', 'finger ' + (on ? 'ok' : 'off') + (info ? ' ' + info.cls : ''));

      var head = el('div', 'finger-head');
      head.appendChild(el('span', 'slot', '#' + f.id));
      // state icons: clock = schedule (green active / amber inactive, hover shows the week), ban = switched off
      var ind = el('div', 'finger-ind');
      if (f.scheduled) {
        var clock = el('span', 'ind ' + (on ? 'ok' : 'wait'));
        clock.appendChild(icon('i-clock'));
        clock.setAttribute('role', 'img');
        clock.setAttribute('aria-label', on ? 'Schedule: active now' : 'Schedule: inactive now');
        clock.onmouseenter = function () { showWeekTip(clock, f); };
        clock.onmouseleave = hideWeekTip;
        clock.onclick = function () { openSchedule(f); };
        ind.appendChild(clock);
      } else if (!on) {
        var ban = el('span', 'ind off');
        ban.appendChild(icon('i-ban'));
        ban.title = 'Disabled';
        ban.setAttribute('role', 'img');
        ban.setAttribute('aria-label', 'Disabled');
        ind.appendChild(ban);
      }
      head.appendChild(ind);
      var sw = el('label', 'switch sm');
      sw.title = f.scheduled ? 'Controlled by the schedule' : on ? 'Enabled - click to disable' : 'Disabled - click to enable';
      if (f.scheduled) sw.classList.add('locked');
      var cb = el('input');
      cb.type = 'checkbox';
      cb.checked = on;
      cb.disabled = !!f.scheduled;
      cb.onchange = function () { setPolicy(f, { enabled: cb.checked ? 1 : 0 }).catch(function (e) { cb.checked = !cb.checked; fail(e); }); };
      sw.appendChild(cb);
      sw.appendChild(el('span', 'track'));
      head.appendChild(sw);

      var body = el('div', 'finger-body');
      if (info) {
        // the action badge sits right next to the name, the finger below
        var line = el('div', 'fn-line');
        line.appendChild(el('div', 'fn-name', info.name));
        line.appendChild(el('div', 'fn-action', info.action));
        body.appendChild(line);
        body.appendChild(el('div', 'fn-finger', info.finger));
      } else {
        body.appendChild(el('div', 'fn-name' + (f.name ? '' : ' unnamed'), f.name || 'unnamed'));
      }

      var foot = el('div', 'finger-foot');
      var schBtn = iconButton('i-clock', 'Schedule', f.scheduled ? 'set' : '', function () { openSchedule(f); }, true);
      if (f.scheduled) {
        schBtn.removeAttribute('title');
        schBtn.onmouseenter = function () { showWeekTip(schBtn, f); };
        schBtn.onmouseleave = hideWeekTip;
      }
      foot.appendChild(schBtn);
      foot.appendChild(iconButton('i-edit', 'Rename', '', function () { openRename(f); }, true));
      foot.appendChild(el('span', 'spacer'));
      foot.appendChild(iconButton('i-trash', 'Delete', 'danger icon-btn', function () {
        confirmDialog('Delete fingerprint #' + f.id + '?', (f.name || 'This fingerprint') + ' will be removed from the sensor.', 'Delete', function () {
          action('/editFingerprints?selectedFingerprint=' + f.id + '&btnDelete=1').then(function () { toast('Deleted'); return loadFingers(); }).catch(fail);
        });
      }, false));

      card.appendChild(head);
      card.appendChild(body);
      card.appendChild(foot);
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
  // one card per LED state; cfg keys for color/animation and the /api/led setting names
  var LED_SLOTS = [
    { id: 'Ready', title: 'Ready', sub: 'Idle, touch ring active.', color: 'touchRingActiveColor', colorApi: 'touchRingColor', seq: 'touchRingActiveSequence', seqApi: 'touchRingSequence' },
    { id: 'Scan', title: 'Finger detected', sub: 'While a finger is on the sensor.', color: 'scanColor', colorApi: 'scanColor', seq: 'scanSequence', seqApi: 'scanSequence' },
    { id: 'Match', title: 'Match', sub: 'Fingerprint was recognized.', color: 'matchColor', colorApi: 'matchColor', seq: 'matchSequence', seqApi: 'matchSequence' },
    { id: 'Ignore', title: 'Touch ring ignored', sub: 'Shown while "Ignore touch ring" is on.', color: 'ignoreColor', colorApi: 'ignoreColor', seq: 'ignoreSequence', seqApi: 'ignoreSequence' },
    { id: 'GongOff', title: 'Gong disabled', sub: 'Shown while the hardware gong is switched off (highest priority).', color: 'gongOffColor', colorApi: 'gongOffColor', seq: 'gongOffSequence', seqApi: 'gongOffSequence' }
  ];

  function buildLed() {
    var host = $('ledCards');
    host.textContent = '';
    LED_SLOTS.forEach(function (slot) {
      var card = el('div', 'card led-card');
      var disc = el('div', 'sensor-disc small');
      disc.id = 'prev' + slot.id;
      disc.setAttribute('data-seq', 3);
      disc.innerHTML = '<svg class="ring-svg" viewBox="0 0 100 100"><circle class="ring-track" cx="50" cy="50" r="42"/><circle class="ring-led" cx="50" cy="50" r="42"/></svg><svg class="fp"><use href="#i-fp"/></svg>';
      var body = el('div', 'led-body');
      body.appendChild(el('h3', '', slot.title));
      body.appendChild(el('p', 'sub', slot.sub));

      var fc = el('div', 'field');
      fc.appendChild(el('div', 'label', 'Color'));
      var box = el('div', 'swatches');
      box.setAttribute('data-led', slot.color);
      for (var i = 1; i <= 7; i++) (function (idx) {
        var b = el('button', 'swatch' + (idx > 3 ? ' legacy' : ''));
        b.type = 'button';
        b.style.setProperty('--c', COLORS[idx].h);
        b.title = COLORS[idx].n + (idx > 3 ? ' (newer sensors only)' : '');
        b.setAttribute('aria-label', COLORS[idx].n);
        b.setAttribute('data-v', idx);
        b.onclick = function () { setLed(slot.colorApi, idx); };
        box.appendChild(b);
      })(i);
      fc.appendChild(box);
      body.appendChild(fc);

      var fs = el('div', 'field');
      fs.appendChild(el('div', 'label', 'Animation'));
      var seg = el('div', 'seg');
      seg.setAttribute('data-led', slot.seq);
      SEQS.forEach(function (sq) {
        var b = el('button', '', sq.n);
        b.type = 'button';
        b.setAttribute('data-v', sq.v);
        b.onclick = function () { setLed(slot.seqApi, sq.v); };
        seg.appendChild(b);
      });
      fs.appendChild(seg);
      body.appendChild(fs);

      card.appendChild(disc);
      card.appendChild(body);
      host.appendChild(card);
    });
  }

  function setLed(setting, value) {
    getJson('/api/led?setting=' + setting + '&value=' + value).then(function (c) { cfg = c; renderLed(); renderStatus(); toast('Saved'); }).catch(fail);
  }

  function renderLed() {
    if (!cfg) return;
    Array.prototype.forEach.call(document.querySelectorAll('#ledCards [data-led]'), function (box) {
      var cur = cfg[box.getAttribute('data-led')];
      Array.prototype.forEach.call(box.children, function (b) { b.classList.toggle('on', parseInt(b.getAttribute('data-v'), 10) === cur); });
    });
    LED_SLOTS.forEach(function (slot) { paintDisc($('prev' + slot.id), cfg[slot.color], cfg[slot.seq]); });
    if (!$('ringDuration').value) $('ringDuration').value = cfg.ringDuration; // later changes belong to the user until saved or discarded
  }

  // ---------- connectivity form ----------
  var FIELDS = [['mqttServer', 'v'], ['mqttUsername', 'v'], ['mqttPassword', 'v'], ['mqttRootTopic', 'v'], ['ntpServer', 'v'], ['timezone', 'v'], ['haDiscovery', 'c'], ['haDiscoveryPrefix', 'v']];

  function fillForm() {
    FIELDS.forEach(function (f) {
      var input = $(f[0]);
      if (f[1] === 'c') input.checked = !!cfg[f[0]];
      else {
        if (input.tagName === 'SELECT' && cfg[f[0]] && !Array.prototype.some.call(input.options, function (o) { return o.value === cfg[f[0]]; }))
          input.appendChild(el('option', '', 'Custom: ' + cfg[f[0]])).value = cfg[f[0]];
        input.value = cfg[f[0]];
      }
    });
    updateTopicHint();
    updateSaveBar();
  }

  function settingsDirty() {
    if (!cfg) return false;
    return FIELDS.some(function (f) {
      var input = $(f[0]);
      return (f[1] === 'c' ? input.checked : input.value) !== cfg[f[0]];
    });
  }

  function ringDirty() {
    if (!cfg) return false;
    var d = parseInt($('ringDuration').value, 10);
    return !isNaN(d) && d !== cfg.ringDuration;
  }

  function formDirty() { return settingsDirty() || ringDirty(); }

  // one save bar for Connectivity and System: changes that need a restart say so, the ring duration applies immediately
  function updateSaveBar(view) {
    view = view || location.hash.replace('#', '');
    if (view === 'settings') view = 'connectivity';
    $('savebar').hidden = !((view === 'connectivity' || view === 'system') && formDirty());
    $('btnSaveAll').textContent = settingsDirty() ? 'Save & restart' : 'Save';
  }

  function updateTopicHint() {
    var r = $('mqttRootTopic').value || '...';
    $('topicHint').textContent = 'Topics: ' + r + '/ring, matchName, matchId, matchConfidence, connection, uptime, ... (commands: ' + r + '/ignoreTouchRing, ringEnabled, reboot)';
  }

  function saveAll() {
    var sd = settingsDirty(), rd = ringDirty();
    var d = parseInt($('ringDuration').value, 10);
    if (rd && !(d >= 50 && d <= 5000)) { toast('Duration must be 50-5000 ms', true); return; }
    if (sd && (!$('mqttServer').value.trim() || !$('mqttRootTopic').value.trim())) { toast('Broker and root topic are required', true); return; }
    var ringSaved = rd ? action('/ring?btnSaveRing=1&ringDuration=' + d + (st && st.ringEnabled ? '&ringEnabled=on' : '')) : Promise.resolve();
    ringSaved.then(function () {
      if (!sd) {
        toast('Saved');
        return Promise.all([getJson('/api/settings').then(function (c) { cfg = c; fillForm(); $('ringDuration').value = cfg.ringDuration; renderLed(); }), loadStatus()]);
      }
      var p = new URLSearchParams();
      p.set('btnSaveSettings', '1');
      p.set('mqtt_server', $('mqttServer').value.trim());
      p.set('mqtt_username', $('mqttUsername').value);
      p.set('mqtt_password', $('mqttPassword').value);
      p.set('mqtt_rootTopic', $('mqttRootTopic').value.trim());
      p.set('ntpServer', $('ntpServer').value.trim());
      p.set('timezone', $('timezone').value);
      if ($('haDiscovery').checked) p.set('haDiscovery', '1');
      p.set('haDiscoveryPrefix', $('haDiscoveryPrefix').value.trim());
      return action('/settings?' + p.toString()).then(function () { restartOverlay('Saving and restarting...'); });
    }).catch(fail);
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
  // windows arrive compact as [days, from, to, allDay]
  function normalizeFingers(list) {
    list.forEach(function (f) {
      f.windows = (f.windows || []).map(function (a) { return { days: a[0], from: a[1], to: a[2], allDay: !!a[3] }; });
    });
    return list;
  }

  function loadFingers() { return getJson('/api/fingerprints').then(function (f) { fingers = normalizeFingers(f); renderFingers(); }); }
  function loadStatus() {
    return getJson('/api/status').then(function (s) { st = s; renderStatus(); }).catch(function () {});
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
    // theme: auto follows the system, light/dark are remembered in this browser
    function applyTheme(t) {
      if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t);
      else { t = 'auto'; document.documentElement.removeAttribute('data-theme'); }
      Array.prototype.forEach.call($('themeSeg').children, function (b) { b.classList.toggle('on', b.getAttribute('data-theme') === t); });
      return t;
    }
    var theme = 'auto';
    try { theme = localStorage.getItem('theme') || 'auto'; } catch (e) {}
    applyTheme(theme);
    Array.prototype.forEach.call($('themeSeg').children, function (b) {
      b.onclick = function () {
        var t = applyTheme(b.getAttribute('data-theme'));
        try { if (t === 'auto') localStorage.removeItem('theme'); else localStorage.setItem('theme', t); } catch (e) {}
      };
    });
    $('btnCheckUpdate').onclick = function () { checkUpdate(true); };
    $('fpSearch').oninput = function () { renderFingers(); };
    $('btnAdd').onclick = openEnroll;
    $('btnEnrollStart').onclick = startEnroll;
    $('btnEnrollDone').onclick = function () { $('dlgEnroll').close(); };
    // closing the dialog (X, Esc, backdrop) while an enrollment is running cancels it on the device
    $('dlgEnroll').addEventListener('close', function () {
      if (!enroll) return;
      enroll = null;
      $('enDisc').classList.remove('busy');
      action('/api/enroll/cancel').then(function () { toast('Enrollment cancelled'); }).catch(fail);
    });
    Array.prototype.forEach.call(document.querySelectorAll('[data-close]'), function (b) {
      b.onclick = function () { b.closest('dialog').close(); };
    });

    FIELDS.forEach(function (f) {
      var ev = f[1] === 'c' ? 'change' : 'input';
      $(f[0]).addEventListener(ev, function () { updateTopicHint(); updateSaveBar(); });
    });
    $('ringDuration').addEventListener('input', function () { updateSaveBar(); });
    $('btnDiscard').onclick = function () { fillForm(); $('ringDuration').value = cfg.ringDuration; updateSaveBar(); };
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
      es.addEventListener('fingerlist', function (e) { try { fingers = normalizeFingers(JSON.parse(e.data)); renderFingers(); } catch (x) { /* ignore */ } });
    }

    Promise.all([getJson('/api/settings'), getJson('/api/fingerprints'), getJson('/api/status')]).then(function (r) {
      cfg = r[0]; fingers = normalizeFingers(r[1]); st = r[2];
      fillForm(); renderLed(); renderFingers(); renderStatus(); route();
    }).catch(function () { route(); fail(new Error('device not reachable')); });

    setInterval(function () { if (!document.hidden) loadStatus(); }, 4000);
  }

  init();
})();
