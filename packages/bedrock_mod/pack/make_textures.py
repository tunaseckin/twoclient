# Draws the menu's nine-slice button textures: OneClient's glass buttons, a thin
# light border over a dark see-through fill. Run it again after changing a color.
import struct, zlib, json, os

SIZE, RADIUS, SLICE = 48, 12, 16
here = os.path.join(os.path.dirname(os.path.abspath(__file__)), "textures/ui/twoclient")

def png(path, pixels):
    raw = b"".join(b"\0" + bytes(sum(row, ())) for row in pixels)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

def coverage(x, y, inset):
    # how much of the pixel lies inside a rounded rect shrunk by inset, 4x4 supersampled
    hits = 0
    for sy in range(4):
        for sx in range(4):
            px, py = x + (sx + .5) / 4, y + (sy + .5) / 4
            r = RADIUS - inset
            cx = min(max(px, inset + r), SIZE - inset - r)
            cy = min(max(py, inset + r), SIZE - inset - r)
            hits += (px - cx) ** 2 + (py - cy) ** 2 <= r * r and inset <= px <= SIZE - inset and inset <= py <= SIZE - inset
    return hits / 16

def button(name, fill, border):
    rows = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            outer, inner = coverage(x, y, 0), coverage(x, y, 1)
            edge = outer - inner
            a = edge * border[3] + inner * fill[3]
            rgb = [(edge * border[3] * border[i] + inner * fill[3] * fill[i]) / a if a else 0 for i in range(3)]
            row.append((*[round(c) for c in rgb], round(a)))
        rows.append(row)
    png(os.path.join(here, name + ".png"), rows)
    with open(os.path.join(here, name + ".json"), "w") as f:
        json.dump({"nineslice_size": SLICE, "base_size": [SIZE, SIZE]}, f)

# OneClient's title screen buttons: a dim fill and a soft light outline
button("button", (20, 18, 28, 120), (225, 228, 255, 125))
button("button_hover", (45, 45, 62, 150), (235, 238, 255, 175))
button("button_pressed", (35, 90, 230, 170), (140, 175, 255, 210))
