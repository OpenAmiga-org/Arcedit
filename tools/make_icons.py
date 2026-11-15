#!/usr/bin/env python3
"""Draws the pictures of the release icons, so the icons are as free as
the rest of arcedit. Public domain.

    python3 -I tools/make_icons.py OUTDIR

writes OUTDIR/<icon>.argb and .sel.argb (64x64 ARGB, normal and selected)
and OUTDIR/mkicons.script, an AmigaDOS script that runs tools/os4/mkicon
in the OS4 guest; icon.library then saves the icons in its own format.
The results are kept in package/icons.
"""
import os
import sys

from PIL import Image, ImageDraw

WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4
SIZE = 64
SS = 4

DARK = (30, 34, 48, 255)
BOX = (196, 150, 86, 255)
BOX_TOP = (222, 184, 122, 255)
BOX_SIDE = (164, 120, 64, 255)
PAPER = (240, 240, 234, 255)
LINE = (120, 120, 130, 255)
PEN = (40, 120, 200, 255)
PEN_TIP = (250, 220, 150, 255)
FOLDER = (236, 196, 96, 255)
FOLDER_BACK = (214, 168, 70, 255)


def canvas():
    im = Image.new("RGBA", (SIZE * SS, SIZE * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def s(*v):
    return [x * SS for x in v]


def finish(im):
    return im.resize((SIZE, SIZE), Image.LANCZOS)


def box(d, x, y, w):
    """An archive: a closed cardboard box seen from the front and above"""
    h = w * 0.7
    t = w * 0.3
    d.polygon(s(x, y + t, x + t, y, x + w + t, y, x + w, y + t), fill=BOX_TOP, outline=DARK)
    d.polygon(s(x + w, y + t, x + w + t, y, x + w + t, y + h, x + w, y + h + t), fill=BOX_SIDE, outline=DARK)
    d.rectangle(s(x, y + t, x + w, y + h + t), fill=BOX, outline=DARK, width=SS)
    d.line(s(x + w / 2, y + t, x + w / 2 + t, y), fill=DARK, width=SS)
    d.rectangle(s(x + w / 2 - 3, y + t, x + w / 2 + 3, y + t + h * 0.45), fill=BOX_TOP, outline=DARK)


def pencil(d, x1, y1, x2, y2, width):
    """A pencil from (x1,y1) (end) to (x2,y2) (tip)"""
    import math
    dx, dy = x2 - x1, y2 - y1
    n = math.hypot(dx, dy)
    ux, uy = dx / n, dy / n
    px, py = -uy * width / 2, ux * width / 2
    tip = 9
    bx, by = x2 - ux * tip, y2 - uy * tip
    d.polygon(s(x1 + px, y1 + py, bx + px, by + py, bx - px, by - py, x1 - px, y1 - py), fill=PEN, outline=DARK)
    d.polygon(s(bx + px, by + py, x2, y2, bx - px, by - py), fill=PEN_TIP, outline=DARK)
    d.polygon(s(x2 - ux * 3 + px * 0.33, y2 - uy * 3 + py * 0.33, x2, y2,
                x2 - ux * 3 - px * 0.33, y2 - uy * 3 - py * 0.33), fill=DARK)


def img_arcedit():
    im, d = canvas()
    box(d, 6, 18, 38)
    pencil(d, 58, 6, 30, 40, 9)
    return finish(im)


def img_drawer():
    im, d = canvas()
    d.rounded_rectangle(s(4, 12, 28, 22), radius=4 * SS, fill=FOLDER_BACK, outline=DARK, width=2 * SS)
    d.rounded_rectangle(s(4, 18, 60, 58), radius=6 * SS, fill=FOLDER, outline=DARK, width=2 * SS)
    return finish(im)


def img_drawer_arcedit():
    im, d = canvas()
    d.rounded_rectangle(s(4, 12, 28, 22), radius=4 * SS, fill=FOLDER_BACK, outline=DARK, width=2 * SS)
    d.rounded_rectangle(s(4, 18, 60, 58), radius=6 * SS, fill=FOLDER, outline=DARK, width=2 * SS)
    box(d, 14, 26, 24)
    pencil(d, 54, 22, 36, 44, 6)
    return finish(im)


def img_text():
    im, d = canvas()
    d.polygon(s(14, 4, 42, 4, 52, 14, 52, 60, 14, 60), fill=PAPER, outline=DARK)
    d.line(s(14, 4, 42, 4, 52, 14, 52, 60, 14, 60, 14, 4), fill=DARK, width=2 * SS)
    d.polygon(s(42, 4, 42, 14, 52, 14), fill=(200, 200, 192, 255), outline=DARK)
    for y in (16, 22, 28, 34, 40, 46, 52):
        d.line(s(20, y, 46 if y != 16 else 36, y), fill=LINE, width=2 * SS)
    return finish(im)


def img_install():
    im, d = canvas()
    box(d, 8, 26, 34)
    d.polygon(s(28, 2, 40, 2, 40, 14, 47, 14, 34, 27, 21, 14, 28, 14), fill=PEN, outline=DARK)
    return finish(im)


ICONS = {
    # file name (without .info): (image, type, default tool, tool types, stack, drawer window)
    "arcedit": (img_drawer_arcedit, WBDRAWER, None, (), None, (80, 60, 360, 160)),
    "Install": (img_install, WBPROJECT, "SYS:Utilities/Installer",
                ("APPNAME=arcedit", "MINUSER=NOVICE", "DEFUSER=NOVICE", "LOGFILE=T:arcedit_install_log"), 32768, None),
    "arcedit.txt": (img_text, WBPROJECT, "SYS:Utilities/MultiView", (), 32768, None),
    "C": (img_drawer, WBDRAWER, None, (), None, (120, 100, 300, 140)),
    "C_arcedit": (img_arcedit, WBTOOL, None, (), 65536, None),
}


def argb(img):
    return b"".join(bytes((a, r, g, b)) for (r, g, b, a) in img.get_flattened_data())


def darker(img):
    out = Image.new("RGBA", img.size)
    out.putdata([(int(r * 0.7), int(g * 0.7), int(b * 0.75), a) for (r, g, b, a) in img.get_flattened_data()])
    return out


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    lines = ["; created by tools/make_icons.py; run in the drawer with the .argb files", "FailAt 21",
             "MakeDir out QUIET"]
    for name, (fn, typ, tool, tts, stack, drawer) in ICONS.items():
        img = fn()
        with open(os.path.join(out, name + ".argb"), "wb") as f:
            f.write(argb(img))
        with open(os.path.join(out, name + ".sel.argb"), "wb") as f:
            f.write(argb(darker(img)))
        t = {WBDRAWER: "DRAWER", WBTOOL: "TOOL", WBPROJECT: "PROJECT"}[typ]
        args = ['"out/%s"' % name, "ARGB=%s.argb" % name, "SELECTED=%s.sel.argb" % name, "TYPE=" + t]
        if tool:
            args.append('"TOOL=%s"' % tool)
        if stack:
            args.append("STACK=%d" % stack)
        if drawer:
            args.append("WINDOW=%d,%d,%d,%d" % drawer)
        for tt in tts:
            args.append('"TT=%s"' % tt)
        lines.append("mkicon " + " ".join(args))
    with open(os.path.join(out, "mkicons.script"), "w") as f:
        f.write("\n".join(lines) + "\n")
    fns = [v[0] for v in ICONS.values()]
    sheet = Image.new("RGBA", (len(fns) * (SIZE + 12) + 8, SIZE + 20), (170, 170, 170, 255))
    for i, fn in enumerate(fns):
        sheet.alpha_composite(fn(), (10 + i * (SIZE + 12), 10))
    sheet.save(os.path.join(out, "icons-preview.png"))


if __name__ == "__main__":
    main()
