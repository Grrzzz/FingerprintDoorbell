"""Builds update.zip (spiffs.bin + firmware.bin, stored) for a release and checks the version.

Usage: python tools/make_update_zip.py vX.Y [build dir]
Run `pio run` and `pio run -t buildfs` first. Output: <build dir>/update.zip
The device unpacks the archive while it is uploaded, so entries must be STORED (no compression)
and spiffs.bin must come first (the filesystem is installed before the firmware).
"""
import os, re, sys, zipfile, hashlib

tag = sys.argv[1] if len(sys.argv) > 1 else ''
build = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '.pio', 'build', 'esp32doit-devkit-v1')
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')

if not re.fullmatch(r'v\d+(\.\d+)*', tag):
    sys.exit('tag must look like v2.1 or v2.0.1')

src = open(os.path.join(root, 'src', 'main.cpp'), encoding='utf-8', errors='replace').read()
m = re.search(r'VersionInfo\s*=\s*"([^"]+)"', src)
if not m:
    sys.exit('VersionInfo not found in src/main.cpp')
version = m.group(1)
if not version.startswith(tag[1:] + ' '):
    sys.exit('VersionInfo "%s" does not match tag %s: change it in src/main.cpp and rebuild' % (version, tag))

fw = open(os.path.join(build, 'firmware.bin'), 'rb').read()
fs = open(os.path.join(build, 'spiffs.bin'), 'rb').read()
if version.encode() not in fw:
    sys.exit('firmware.bin does not contain "%s": it is stale, rebuild (touch src/main.cpp)' % version)
if tag[1:].encode() not in fs and b'app.js' not in fs:
    sys.exit('spiffs.bin looks wrong, run `pio run -t buildfs`')

out = os.path.join(build, 'update.zip')
with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
    z.writestr('spiffs.bin', fs)
    z.writestr('firmware.bin', fw)
print('wrote', os.path.abspath(out))
for name, data in (('update.zip', open(out, 'rb').read()), ('firmware.bin', fw), ('spiffs.bin', fs)):
    print('%s  %9d bytes  sha256 %s' % (name, len(data), hashlib.sha256(data).hexdigest()))