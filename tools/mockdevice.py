"""Virtual FingerprintDoorbell for documentation screenshots.

Serves the web interface from ../data and answers the JSON API with made up data
(no real device, names, addresses or credentials involved).
Usage: python mockdevice.py [port] [version]
The optional version simulates the installed firmware, e.g. "Develop: develop @ 1a2b3c4" or "2.1 Grrzzz+" (dev/modified builds can always update).
Demo states via query string: /?demo=enroll|schedule|picker|hover|update&theme=dark
Reboot, settings save, factory reset and OTA (GitHub self-update and manual upload) are simulated:
the device answers 503 while it restarts, the GitHub release check is answered locally with v9.9.
"""
import json, os, re, sys, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
VERSION = sys.argv[2] if len(sys.argv) > 2 else '2.1 Grrzzz'
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')
TYPES = {'.html': 'text/html', '.js': 'application/javascript', '.css': 'text/css'}

W = lambda days, f, t, a=0: [days, f, t, a]
FINGERS = [
    dict(id=1, name='Alice (Thumb right) #Front door'),
    dict(id=2, name='Alice (Index left) #Garage'),
    dict(id=3, name='Bob (Thumb right) #Front door'),
    dict(id=4, name='Bob (Middle right) #Garage'),
    dict(id=5, name='Carol (Thumb right) #Cleaning', scheduled=True, active=True, windows=[W(5, 480, 720), W(10, 840, 1080)]),
    dict(id=6, name='Dave (Index right) #Guest'),
    dict(id=7, name='Erin - spare key'),
    dict(id=8, name='Frank (Thumb left) #Front door', enabled=False, active=False),
    dict(id=9, name='Gina (Index left) #Cleaning', scheduled=True, active=False, windows=[W(31, 480, 720)]),
    dict(id=10, name='Heidi (Thumb right) #Garage'),
    dict(id=11, name='Ivan (Middle left) #Front door'),
    dict(id=12, name='Judy (Index right) #Cleaning', scheduled=True, active=True, windows=[W(127, 0, 0, 1)]),
]
for f in FINGERS:
    f.setdefault('enabled', True)
    f.setdefault('scheduled', False)
    f.setdefault('active', True)
    f.setdefault('windows', [])

LOG = [
    '[2026-11-03 08:00:12 CET]: System booted successfully!',
    '[2026-11-03 12:44:03 CET]: ringEnabled is now \'on\'',
    '[2026-11-03 13:02:15 CET]: Match Found: 1 - Alice (Thumb right) #Front door with confidence of 287',
    '[2026-11-03 14:20:40 CET]: Fingerprint #8 (Frank (Thumb left) #Front door) is currently disabled, treated as unknown finger.',
    '[2026-11-03 14:20:41 CET]: No Match Found (Code 9)',
    '[2026-11-03 14:31:58 CET]: Match Found: 3 - Bob (Thumb right) #Front door with confidence of 311',
]

STATUS = {
    'hostname': 'FingerprintDoorbell', 'version': '2.1 Grrzzz', 'uptime': 3 * 86400 + 5 * 3600 + 1260, 'rssi': -52, 'ssid': 'MyWiFi',
    'ip': '192.168.1.50', 'heap': 218000, 'mode': 'scan', 'sensorConnected': True, 'pairingValid': True, 'mqtt': 'connected',
    'mqttServer': 'mqtt.example.local:1883', 'haDiscovery': True, 'ignoreTouch': False, 'ringEnabled': True, 'fingerCount': len(FINGERS),
    'storage': {'used': 230, 'total': 630},
    'sensor': {'capacity': 200, 'security': 3, 'templates': 12, 'packetLen': 128, 'baud': 57600, 'status': 12, 'address': 4294967295},
    'sys': {'chip': 'ESP32', 'rev': 1, 'cores': 2, 'mhz': 240, 'flash': 4194304, 'heapMin': 152000, 'heapBlock': 110000, 'heapTotal': 295000, 'temp': 47.5,
            'sdk': 'v3.3.5', 'build': 'Nov 17 2026 10:00:00', 'mac': 'AA:BB:CC:00:11:22', 'channel': 6, 'gateway': '192.168.1.1',
            'netmask': '255.255.255.0', 'dns': '192.168.1.1', 'bssid': 'DE:AD:BE:EF:00:01'}, 'clock': {'day': 1, 'min': 14 * 60 + 32, 'iso': '2026-11-17T14:32:09+0100'},
    'lastEvent': {'type': 'match', 'name': 'Bob (Thumb right) #Front door', 'id': 3, 'confidence': 311, 'time': '2026-11-03 14:31:58 CET'},
    'log': LOG,
}
SETTINGS = {
    'mqttServer': 'mqtt.example.local', 'mqttUsername': 'doorbell', 'mqttPassword': 'secret', 'mqttRootTopic': 'fingerprintDoorbell',
    'ntpServer': 'pool.ntp.org', 'timezone': 'CET-1CEST,M3.5.0,M10.5.0/3', 'haDiscovery': True, 'haDiscoveryPrefix': 'homeassistant',
    'touchRingActiveColor': 7, 'touchRingActiveSequence': 3, 'scanColor': 7, 'matchColor': 4, 'scanSequence': 1, 'matchSequence': 3,
    'ignoreColor': 6, 'ignoreSequence': 3, 'gongOffColor': 1, 'gongOffSequence': 3, 'ringEnabled': True, 'ringDuration': 275,
}
# simulated restart and self-update (timestamps of time.time())
SIM = {'offline_until': 0, 'ota_start': 0, 'ota_kind': ''}
OTA_STEPS = [(2, 'checking', 'Downloading update.zip'), (8, 'filesystem', 'Installing the web interface'), (16, 'firmware', 'Installing the firmware')]


def reboot(seconds=8):
    SIM['offline_until'] = time.time() + seconds


def offline():
    return time.time() < SIM['offline_until']


def ota_status():
    if not SIM['ota_start']:
        return {'state': 'idle', 'percent': 0, 'message': ''}
    t = time.time() - SIM['ota_start']
    for end, state, msg in OTA_STEPS:
        if t < end:
            pct = 1 if state == 'checking' else 2 + int(96 * (t - OTA_STEPS[0][0]) / (OTA_STEPS[-1][0] - OTA_STEPS[0][0]))
            return {'state': state, 'percent': pct, 'message': msg}
    SIM['ota_start'] = 0
    reboot()
    return {'state': 'restarting', 'percent': 100, 'message': 'Update installed, restarting'}


KEYS = {'touchRingColor': 'touchRingActiveColor', 'touchRingSequence': 'touchRingActiveSequence'}

DEMOS = {
    'enroll': "setTimeout(function(){document.getElementById('btnAdd').click();setTimeout(function(){document.getElementById('enName').value='Kim (Thumb right) #Front door';document.getElementById('btnEnrollStart').click();},500);},1500);",
    'schedule': "setTimeout(function(){var c=document.querySelectorAll('.finger')[4];[].slice.call(c.querySelectorAll('.finger-foot button')).filter(function(b){return b.textContent==='Schedule';})[0].click();},1800);",
    'picker': "setTimeout(function(){var c=document.querySelectorAll('.finger')[4];[].slice.call(c.querySelectorAll('.finger-foot button')).filter(function(b){return b.textContent==='Schedule';})[0].click();setTimeout(function(){document.querySelectorAll('.rule')[1].querySelectorAll('.time-val')[0].click();},500);},1800);",
    'hover': "setTimeout(function(){var i=document.querySelectorAll('.finger')[4].querySelector('.ind');i.dispatchEvent(new MouseEvent('mouseenter'));},1800);",
    'update': "setTimeout(function(){var b=document.getElementById('sysUpdate');b.className='d new';b.textContent='Update available: 2.2 Grrzzz - ';var a=document.createElement('a');a.textContent='view release';a.href='#';b.appendChild(a);var g=document.createElement('button');g.className='btn sm';g.style.marginLeft='10px';g.textContent='Update now';b.appendChild(g);},4000);",
}


def page(query):
    STATUS['version'] = VERSION
    STATUS['mode'] = 'scan'
    STATUS['log'] = list(LOG)
    html = open(os.path.join(DATA, 'app.html'), encoding='utf-8').read()
    q = urllib.parse.parse_qs(query)
    head = ''
    if q.get('theme'):
        head = "<script>try{localStorage.setItem('theme','%s')}catch(e){}</script>" % q['theme'][0]
    tail = ''
    demo = q.get('demo', [''])[0]
    if demo in DEMOS:
        tail = '<script>' + DEMOS[demo] + '</script>'
    if q.get('hash'):
        tail += "<script>location.hash='#%s'</script>" % q['hash'][0]
    html = html.replace('<head>', '<head>' + head, 1).replace('</body>', tail + '</body>', 1)
    return html.encode('utf-8')


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype='application/json'):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def device_action(self, path, q):
        arg = lambda k: q.get(k, [''])[0]
        if path == '/reboot':
            reboot()
        elif path == '/factoryReset':
            reboot(60)
        elif path == '/settings':
            SETTINGS.update(mqttServer=arg('mqtt_server'), mqttUsername=arg('mqtt_username'), mqttPassword=arg('mqtt_password'),
                            mqttRootTopic=arg('mqtt_rootTopic'), ntpServer=arg('ntpServer'), timezone=arg('timezone'),
                            haDiscovery=arg('haDiscovery') == '1', haDiscoveryPrefix=arg('haDiscoveryPrefix'))
            STATUS['haDiscovery'] = SETTINGS['haDiscovery']
            reboot()
        elif path == '/ring' and arg('ringDuration'):
            SETTINGS['ringDuration'] = int(arg('ringDuration'))
        elif path == '/deleteAllFingerprints':
            FINGERS.clear()
        elif path == '/editFingerprints':
            f = [x for x in FINGERS if x['id'] == int(arg('selectedFingerprint') or 0)]
            if f and arg('btnDelete'): FINGERS.remove(f[0])
            if f and arg('btnRename'): f[0]['name'] = arg('renameNewName')
        STATUS['fingerCount'] = len(FINGERS)
        return self.j({'ok': True})

    def j(self, obj):
        self.send(200, json.dumps(obj).encode())

    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        data = self.rfile.read(n) if n else b''
        if self.path.split('?')[0] == '/api/update/upload':
            ok = b'filename=' in data[:400] and (b'.zip' in data[:400] or b'.bin' in data[:400])
            if ok:
                time.sleep(3)  # the device installs before it answers
                reboot()
            return self.send(200 if ok else 400, b'{"ok":true}' if ok else b'choose update.zip or firmware.bin', 'application/json' if ok else 'text/plain')
        self.send(404, b'not found', 'text/plain')

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        path, q = u.path, urllib.parse.parse_qs(u.query)
        if path == '/':
            return self.send(200, page(u.query), 'text/html')
        if path == '/events':
            return self.send(204, b'')
        if offline() and path.startswith('/api/'):
            return self.send(503, b'restarting', 'text/plain')
        if path == '/api/status':
            STATUS['version'] = VERSION
            STATUS['update'] = ota_status()
            return self.j(STATUS)
        if path == '/mock/releases/latest':
            return self.j({'tag_name': 'v9.9', 'html_url': 'https://github.com/Grrzzz/FingerprintDoorbell/releases'})
        if path == '/api/selfupdate/start':
            if SIM['ota_start']:
                return self.send(409, b'an update is already running', 'text/plain')
            SIM['ota_start'] = time.time()
            return self.j({'ok': True})
        if path in ('/reboot', '/pairing', '/deleteAllFingerprints', '/factoryReset', '/settings', '/ring', '/editFingerprints'):
            return self.device_action(path, q)
        if path == '/api/fingerprints':
            return self.j(FINGERS)
        if path == '/api/settings':
            return self.j(SETTINGS)
        if path == '/api/storage':
            return self.j({'total': 630, 'used': 230, 'reserved': 126, 'fingers': 31, 'settings': 39, 'other': 160, 'fingerCount': len(FINGERS)})
        if path == '/api/led':
            key = q.get('setting', [''])[0]
            SETTINGS[KEYS.get(key, key)] = int(q.get('value', ['0'])[0])
            return self.j(SETTINGS)
        if path == '/api/control':
            if 'ringEnabled' in q: STATUS['ringEnabled'] = q['ringEnabled'][0] == 'on'
            if 'ignoreTouchRing' in q: STATUS['ignoreTouch'] = q['ignoreTouchRing'][0] == 'on'
            return self.j(STATUS)
        if path == '/api/finger/policy':
            f = [x for x in FINGERS if x['id'] == int(q['id'][0])][0]
            if 'enabled' in q: f['enabled'] = q['enabled'][0] == '1'
            if 'scheduled' in q: f['scheduled'] = q['scheduled'][0] == '1'
            if 'w' in q:
                f['windows'] = [[int(v) for v in w.split(',')] for w in q['w'][0].split(';') if w]
            return self.j(FINGERS)
        if path == '/enroll':
            STATUS['mode'] = 'enroll'
            STATUS['log'] = LOG + ['[2026-11-03 14:35:02 CET]: Enrollment for id #13 started. We need to scan your finger 5 times until enrollment is completed.',
                                   '[2026-11-03 14:35:10 CET]: Take #3 (place your finger on the sensor until led ring stops flashing, then remove it).']
            return self.j({'ok': True})
        if path == '/api/enroll/cancel':
            STATUS['mode'] = 'scan'
            return self.j({'ok': True})
        name = path.lstrip('/')
        f = os.path.join(DATA, name)
        if os.path.isfile(f):
            body = open(f, 'rb').read()
            if name == 'app.js':  # answer the GitHub release check locally
                body = body.replace(b'https://api.github.com/repos/Grrzzz/FingerprintDoorbell/releases/latest', b'/mock/releases/latest')
            return self.send(200, body, TYPES.get(os.path.splitext(name)[1], 'application/octet-stream'))
        self.send(404, b'not found', 'text/plain')


ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
