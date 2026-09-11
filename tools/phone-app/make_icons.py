from PIL import Image, ImageDraw
import os

def make_icon(size, path):
    img = Image.new('RGBA', (size, size), (15, 15, 26, 255))
    d = ImageDraw.Draw(img)
    # bg circle
    d.ellipse([4, 4, size-4, size-4], fill=(30, 144, 255, 255))
    # keyboard 3x5 mini grid
    cols, rows = 5, 3
    cell = (size - 20) // 5
    ox = (size - cell * 5) // 2
    oy = (size - cell * 3) // 2
    for r in range(rows):
        for c in range(cols):
            x0 = ox + c * cell + 1
            y0 = oy + r * cell + 1
            x1 = x0 + cell - 2
            y1 = y0 + cell - 2
            d.rectangle([x0, y0, x1, y1], fill=(255, 255, 255, 220))
    img.save(path, 'PNG')

out = os.path.dirname(__file__)
make_icon(192, os.path.join(out, 'icon-192.png'))
make_icon(512, os.path.join(out, 'icon-512.png'))
print('icons OK')
