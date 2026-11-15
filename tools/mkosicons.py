#!/usr/bin/env python3
"""Makes the three system icons the release notes' download table shows, from each system's own
logo, the way PiArcBoot makes a game's icon from its sprite (piarcboot icon): the logo in a round
frame over a blurred, lightened copy of its colours.

    python3 tools/mkosicons.py <piarcboot>    writes docs/images/os-raspberrypi.png, os-windows.png
                                             and os-linux.png, with Pillow

The logos are fetched from Wikimedia's renders of them, trimmed to what they show and made 440
pixels on their longer side. The Windows logo is solid to its edges, so it is given a margin held
by four nearly clear corner pixels, which the circle never reaches, and its panes sit inside the
circle as the other two logos do. docs/images/README.md says whose each logo is."""
import os
import subprocess
import sys
import tempfile
import urllib.request

from PIL import Image

LOGOS = {
    "raspberrypi": ("https://upload.wikimedia.org/wikipedia/en/thumb/c/cb/Raspberry_Pi_Logo.svg/500px-Raspberry_Pi_Logo.svg.png", 1.0),
    "windows": ("https://upload.wikimedia.org/wikipedia/commons/thumb/8/87/Windows_logo_-_2021.svg/500px-Windows_logo_-_2021.svg.png", 0.70),
    "linux": ("https://upload.wikimedia.org/wikipedia/commons/thumb/3/35/Tux.svg/500px-Tux.svg.png", 1.0),
}
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "images")

piarcboot = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/piarcboot")
os.makedirs(OUT, exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    for name, (url, share) in LOGOS.items():
        raw = os.path.join(tmp, name + ".png")
        req = urllib.request.Request(url, headers={"User-Agent": "PiArcBoot-icons/1.0"})
        open(raw, "wb").write(urllib.request.urlopen(req).read())
        im = Image.open(raw).convert("RGBA")
        im = im.crop(im.getbbox())
        k = 440 / max(im.size)
        im = im.resize((round(im.size[0] * k), round(im.size[1] * k)), Image.LANCZOS)
        if share < 1:
            side = round(max(im.size) / share)
            pad = Image.new("RGBA", (side, side), (0, 0, 0, 0))
            pad.paste(im, ((side - im.size[0]) // 2, (side - im.size[1]) // 2))
            colour = im.getpixel((im.size[0] // 4, im.size[1] // 4))[:3]
            for xy in ((0, 0), (side - 1, 0), (0, side - 1), (side - 1, side - 1)):
                pad.putpixel(xy, colour + (1,))
            im = pad
        im.save(raw)
        dest = os.path.join(OUT, "os-%s.png" % name)
        subprocess.run([piarcboot, "icon", raw, dest], check=True, stdout=subprocess.DEVNULL)
        print(os.path.relpath(dest))
