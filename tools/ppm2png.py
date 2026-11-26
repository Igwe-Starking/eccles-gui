#!/usr/bin/env python3
"""ppm2png.py - turn the P6 .ppm that the host examples write into a .png (needs Pillow)

  python3 tools/ppm2png.py demo.ppm demo.png [scale]
"""
import sys
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
im = Image.open(src)
if scale > 1:
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
im.save(dst)
print("wrote", dst, im.size)
