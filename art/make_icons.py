"""Train window button icons, in the game's button frame: a locomotive pushing
its wagons (reversing), a wagon under a number the plugin writes (wagons from
a supply base); and the two Scripts menu buttons (all freight / all passenger
trains), 512 px.

    python make_icons.py <game button_depo.png> <out dir>
Writes button_push_off/on.png and button_limit_off/on.png (off a dark symbol,
on a green one), RR_AllFreight.png and RR_AllPassenger.png.
"""
import sys

from PIL import Image, ImageDraw

SIZE, K = 96, 4                      # the game's buttons are 96 px; drawn 4x, then scaled down


def frame(path):
    im = Image.open(path).convert("RGBA")
    # the depot button without its house: its paper, light in the middle and
    # darker towards the frame (colours sampled from the button)
    px = im.load()
    mid, edge = (252, 235, 197), (247, 208, 160)
    for y in range(15, 82):
        for x in range(15, 82):
            r = min(1.0, max(abs(x - 48), abs(y - 48)) / 33) ** 2
            px[x, y] = tuple(int(mid[i] * (1 - r) + edge[i] * r) for i in range(3)) + (255,)
    return im


def push(color):
    big = Image.new("RGBA", (SIZE * K, SIZE * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    s = K
    # a locomotive (cab on its right end) pushing two wagons to the left,
    # an arrow over them pointing the way they go
    for x0 in (12, 36):
        d.rounded_rectangle([x0 * s, 46 * s, (x0 + 21) * s, 62 * s], radius=2 * s, fill=color)
        for wx in (x0 + 5, x0 + 16):
            d.ellipse([(wx - 3.5) * s, 62 * s, (wx + 3.5) * s, 69 * s], fill=color)
    d.rounded_rectangle([60 * s, 44 * s, 84 * s, 62 * s], radius=2 * s, fill=color)
    d.rectangle([74 * s, 36 * s, 84 * s, 46 * s], fill=color)
    for wx in (66, 78):
        d.ellipse([(wx - 3.5) * s, 62 * s, (wx + 3.5) * s, 69 * s], fill=color)
    d.polygon([(14 * s, 27 * s), (26 * s, 20 * s), (26 * s, 34 * s)], fill=color)
    d.rectangle([26 * s, 25 * s, 70 * s, 29 * s], fill=color)
    # drawn edge to edge: shrunk to 3/4 so it keeps clear of the frame
    small = big.resize((SIZE * 3 // 4, SIZE * 3 // 4), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(small, (SIZE // 8, SIZE // 8 + 3))
    return out


def limit(color):
    big = Image.new("RGBA", (SIZE * K, SIZE * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    s = K
    # a wagon below; the plugin writes the number of wagons above it
    d.rounded_rectangle([28 * s, 56 * s, 68 * s, 72 * s], radius=2 * s, fill=color)
    for wx in (36, 60):
        d.ellipse([(wx - 3.5) * s, 72 * s, (wx + 3.5) * s, 79 * s], fill=color)
    return big.resize((SIZE, SIZE), Image.LANCZOS)


def menu_icon(base, passenger):
    """The Scripts menu buttons, 512 px: a freight or a passenger train pushed
    to the left by its locomotive."""
    big = Image.new("RGBA", (SIZE * K, SIZE * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    s = K
    color = (60, 57, 48, 255)
    for x0 in (10, 35):
        d.rounded_rectangle([x0 * s, 42 * s, (x0 + 22) * s, 62 * s], radius=2 * s, fill=color)
        if passenger:                       # a coach: a row of windows
            for wx in range(x0 + 3, x0 + 20, 5):
                d.rectangle([wx * s, 46 * s, (wx + 3) * s, 51 * s], fill=(252, 235, 197, 255))
        for wx in (x0 + 5, x0 + 17):
            d.ellipse([(wx - 3.5) * s, 62 * s, (wx + 3.5) * s, 69 * s], fill=color)
    d.rounded_rectangle([60 * s, 44 * s, 86 * s, 62 * s], radius=2 * s, fill=color)
    d.rectangle([76 * s, 34 * s, 86 * s, 46 * s], fill=color)
    for wx in (66, 80):
        d.ellipse([(wx - 3.5) * s, 62 * s, (wx + 3.5) * s, 69 * s], fill=color)
    d.polygon([(12 * s, 26 * s), (25 * s, 18 * s), (25 * s, 34 * s)], fill=color)
    d.rectangle([25 * s, 24 * s, 72 * s, 28 * s], fill=color)
    small = big.resize((SIZE * 3 // 4, SIZE * 3 // 4), Image.LANCZOS)   # clear of the frame
    im = base.copy()
    im.alpha_composite(small, (SIZE // 8, SIZE // 8 + 3))
    return im.resize((512, 512), Image.LANCZOS)


def main():
    base = frame(sys.argv[1])
    out = sys.argv[2].rstrip("/\\")
    for kind, symbol in (("push", push), ("limit", limit)):
        for name, color in (("off", (60, 57, 48, 255)), ("on", (40, 150, 40, 255))):
            im = base.copy()
            im.alpha_composite(symbol(color))
            im.save(f"{out}/button_{kind}_{name}.png")
    menu_icon(base, False).save(f"{out}/RR_AllFreight.png")
    menu_icon(base, True).save(f"{out}/RR_AllPassenger.png")


if __name__ == "__main__":
    main()
