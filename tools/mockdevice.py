"""Virtual FingerprintDoorbell for documentation screenshots.

Serves the web interface from ../data and answers the JSON API with made up data
(no real device, names, addresses or credentials involved).
Usage: python mockdevice.py [port]
Demo states via query string: /?demo=enroll|schedule|picker|hover|update&theme=dark
"""
import json, os, re, sys, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
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
    'update': {'state': 'idle', 'percent': 0, 'message': ''}, 'resetReason': 'power on',
    'storage': {'used': 230, 'total': 630}, 'clock': {'day': 1, 'min': 14 * 60 + 32},
    'lastEvent': {'type': 'match', 'name': 'Bob (Thumb right) #Front door', 'id': 3, 'confidence': 311, 'time': '2026-11-03 14:31:58 CET'},
    'log': LOG,
}
SETTINGS = {
    'mqttServer': 'mqtt.example.local', 'mqttUsername': 'doorbell', 'mqttPassword': 'secret', 'mqttRootTopic': 'fingerprintDoorbell',
    'ntpServer': 'pool.ntp.org', 'timezone': 'CET-1CEST,M3.5.0,M10.5.0/3', 'haDiscovery': True, 'haDiscoveryPrefix': 'homeassistant',
    'touchRingActiveColor': 7, 'touchRingActiveSequence': 3, 'scanColor': 7, 'matchColor': 4, 'scanSequence': 1, 'matchSequence': 3,
    'ignoreColor': 6, 'ignoreSequence': 3, 'gongOffColor': 1, 'gongOffSequence': 3, 'ringEnabled': True, 'ringDuration': 275,
}
KEYS = {'touchRingColor': 'touchRingActiveColor', 'touchRingSequence': 'touchRingActiveSequence'}

DEMOS = {
    'enroll': "setTimeout(function(){document.getElementById('btnAdd').click();setTimeout(function(){document.getElementById('enName').value='Kim (Thumb right) #Front door';document.getElementById('btnEnrollStart').click();},500);},1500);",
    'schedule': "setTimeout(function(){var c=document.querySelectorAll('.finger')[4];[].slice.call(c.querySelectorAll('.finger-foot button')).filter(function(b){return b.textContent==='Schedule';})[0].click();},1800);",
    'picker': "setTimeout(function(){var c=document.querySelectorAll('.finger')[4];[].slice.call(c.querySelectorAll('.finger-foot button')).filter(function(b){return b.textContent==='Schedule';})[0].click();setTimeout(function(){document.querySelectorAll('.rule')[1].querySelectorAll('.time-val')[0].click();},500);},1800);",
    'hover': "setTimeout(function(){var i=document.querySelectorAll('.finger')[4].querySelector('.ind');i.dispatchEvent(new MouseEvent('mouseenter'));},1800);",
    'update': "setTimeout(function(){var b=document.getElementById('sysUpdate');b.className='d new';b.textContent='Update available: 2.2 Grrzzz - ';var a=document.createElement('a');a.textContent='view release';a.href='#';b.appendChild(a);var g=document.createElement('button');g.className='btn sm';g.style.marginLeft='10px';g.textContent='Update now';b.appendChild(g);},4000);",
}


def page(query):
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

    def j(self, obj):
        self.send(200, json.dumps(obj).encode())

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        path, q = u.path, urllib.parse.parse_qs(u.query)
        if path == '/':
            return self.send(200, page(u.query), 'text/html')
        if path == '/events':
            return self.send(204, b'')
        if path == '/api/status':
            return self.j(STATUS)
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
            return self.send(200, open(f, 'rb').read(), TYPES.get(os.path.splitext(name)[1], 'application/octet-stream'))
        self.send(404, b'not found', 'text/plain')


ThreadingHTTPServer(('127.0.0.1', PORT), H).serve_forever()
