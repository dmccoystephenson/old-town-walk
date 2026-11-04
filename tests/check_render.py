"""Fails on a blank or uniform frame: `python tests/check_render.py shot.png`."""
import sys

from PIL import Image

img = Image.open(sys.argv[1]).convert("RGB")
colours = len(set(img.resize((160, 90)).get_flattened_data()))
print(f"{img.width}x{img.height}, {colours} distinct colours")
sys.exit(0 if colours > 200 else 1)
