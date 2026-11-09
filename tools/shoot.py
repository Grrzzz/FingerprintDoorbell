"""Takes the documentation screenshots of the virtual device with headless Edge (run mockdevice.py first)."""
import os, subprocess, sys, tempfile, time
from PIL import Image, ImageChops

PORT = sys.argv[1] if len(sys.argv) > 1 else '8090'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'shots'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
os.makedirs(OUT, exist_ok=True)

# name, query, width, height
SHOTS = [
    ('ui-overview', '?hash=overview', 1280, 1000),
    ('ui-fingerprints', '?hash=fingerprints', 1280, 1500),
    ('ui-fingerprint-schedule', '?hash=fingerprints&demo=schedule', 1280, 960),
    ('ui-fingerprint-picker', '?hash=fingerprints&demo=picker', 1280, 960),
    ('ui-fingerprint-hover', '?hash=fingerprints&demo=hover', 1280, 700),
    ('ui-enroll', '?hash=fingerprints&demo=enroll', 1280, 800),
    ('ui-led', '?hash=led', 1280, 1700),
    ('ui-connectivity', '?hash=connectivity', 1280, 1000),
    ('ui-system', '?hash=system&demo=update', 1280, 1500),
    ('ui-dark', '?hash=fingerprints&theme=dark', 1280, 1100),
    ('ui-mobile', '?hash=overview', 500, 1100),
]


def trim(path):
    # crop the empty page area below the content (the sidebar is ignored, it always reaches the bottom)
    im = Image.open(path).convert('RGB')
    main = im.crop((270, 0, im.width, im.height)) if im.width > 700 else im
    bg = Image.new('RGB', main.size, main.getpixel((main.width - 1, main.height - 1)))
    box = ImageChops.difference(main, bg).getbbox()
    if box:
        im.crop((0, 0, im.width, min(im.height, box[3] + 40))).save(path)


for name, query, w, h in SHOTS:
    out = os.path.abspath(os.path.join(OUT, name + '.png'))
    if os.path.exists(out):
        os.remove(out)
    for attempt in range(8):
        prof = tempfile.mkdtemp()
        subprocess.run([EDGE, '--headless=new', '--no-first-run', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1', '--user-data-dir=' + prof,
                        '--window-size=%d,%d' % (w, h), '--virtual-time-budget=9000', '--screenshot=' + out,
                        'http://127.0.0.1:%s/%s' % (PORT, query)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)
        time.sleep(1.5)
        if os.path.exists(out):
            break
    if os.path.exists(out):
        if name not in ('ui-fingerprint-schedule', 'ui-fingerprint-picker', 'ui-fingerprint-hover', 'ui-enroll'):
            trim(out)
        print(name, Image.open(out).size)
    else:
        print(name, 'FAILED')
