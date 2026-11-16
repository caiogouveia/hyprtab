#!/usr/bin/env python3
"""Gera o tema de cursores "sgi-red": pixel art vermelha com contorno branco,
no estilo dos cursores do IRIX/4Dwm da SGI.

Só usa a biblioteca padrão. Saída (um diretório de tema de ícones):
  <out>/index.theme            tema XCursor
  <out>/cursors/*              XCursor (+ symlinks com os nomes alternativos)
  <out>/manifest.hl            tema hyprcursor
  <out>/hyprcursors/*.hlc      hyprcursor (zip com meta.hl + PNGs)

Cada glifo é desenhado em uma grade de 1 px no tamanho nominal 24 (ver ART/funções
abaixo): 'X' = cor principal, 'o' = branco interno, '.' = transparente. O contorno
branco externo de 1 px é gerado sozinho. Os tamanhos maiores são o mesmo bitmap
ampliado por fator inteiro (nearest), para manter o visual de pixel art.
"""
import argparse, io, math, os, shutil, struct, sys, zipfile, zlib

BASE = 24
SCALES = (1, 2, 3, 4)  # 24, 48, 72, 96
SHADOW = 90             # opacidade da sombra (0 desliga)

# ------------------------------------------------------------------ grade


class G:
    """Grade de caracteres com hotspot."""

    def __init__(self, w, h, hot=(0, 0)):
        self.w, self.h, self.hot = w, h, hot
        self.px = [['.'] * w for _ in range(h)]

    @staticmethod
    def art(text, hot):
        rows = [r for r in text.strip('\n').split('\n')]
        rows = [r.strip() for r in rows]
        w = max(len(r) for r in rows)
        g = G(w, len(rows), hot)
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                g.px[y][x] = c
        return g

    @staticmethod
    def fn(w, h, f, hot):
        g = G(w, h, hot)
        for y in range(h):
            for x in range(w):
                c = f(x, y)
                if c:
                    g.px[y][x] = c
        return g

    def get(self, x, y):
        return self.px[y][x] if 0 <= x < self.w and 0 <= y < self.h else '.'

    def copy(self):
        g = G(self.w, self.h, self.hot)
        g.px = [r[:] for r in self.px]
        return g

    def over(self, src, ox, oy):
        """Cola src em (ox,oy), crescendo a grade se precisar. Os pixels de src
        recebem um halo transparente de 1 px para o contorno separar as peças."""
        w, h = max(self.w, ox + src.w + 1), max(self.h, oy + src.h + 1)
        g = G(w, h, self.hot)
        for y in range(self.h):
            g.px[y][:self.w] = self.px[y][:]
        for y in range(src.h):
            for x in range(src.w):
                if src.px[y][x] != '.':
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            if (dx or dy) and src.get(x + dx, y + dy) == '.':
                                tx, ty = ox + x + dx, oy + y + dy
                                if 0 <= tx < w and 0 <= ty < h:
                                    g.px[ty][tx] = '_'  # buraco: vira contorno
        for y in range(src.h):
            for x in range(src.w):
                if src.px[y][x] != '.':
                    g.px[oy + y][ox + x] = src.px[y][x]
        return g

    def flip_h(self):
        g = G(self.w, self.h, (self.w - 1 - self.hot[0], self.hot[1]))
        g.px = [r[::-1] for r in self.px]
        return g

    def flip_v(self):
        g = G(self.w, self.h, (self.hot[0], self.h - 1 - self.hot[1]))
        g.px = [r[:] for r in self.px[::-1]]
        return g

    def transpose(self):
        g = G(self.h, self.w, (self.hot[1], self.hot[0]))
        g.px = [[self.px[y][x] for y in range(self.h)] for x in range(self.w)]
        return g


# ------------------------------------------------------------------ glifos

ARROW = G.art('''
X...........
XX..........
XXX.........
XXXX........
XXXXX.......
XXXXXX......
XXXXXXX.....
XXXXXXXX....
XXXXXXXXX...
XXXXXXXXXX..
XXXXXXXXXXX.
XXXXXXXXXXXX
XXXXXXXXXX..
XXXXXXXX....
XXXXXXXXX...
XXXX.XXXX...
XX....XXXX..
......XXXX..
.......XXXX.
.......XXXX.
''', (0, 0))

HAND = G.art('''
.....XX..........
....XooX.........
....XooX.........
....XooX.........
....XooXXX.......
....XooXooXXX....
....XooXooXooXX..
....XooXooXooXoX.
.XX.XooXooXooXoX.
XooXXooooooooooX.
XoooXooooooooooX.
.XooXooooooooooX.
.XoooooooooooooX.
..XooooooooooooX.
..XoooooooooooX..
...XooooooooooX..
...XoooooooooX...
....XooooooooX...
....XooooooooX...
....XXXXXXXXXX...
''', (5, 0))

FIST = G.art('''
....XX.XX.XX....
...XooXooXooXX..
...XooXooXooXoX.
.XXXooXooXooXoX.
XooXooooooooooX.
XoooooooooooooX.
.XooooooooooooX.
..XoooooooooooX.
...XoooooooooX..
....XooooooooX..
....XooooooooX..
....XXXXXXXXXX..
''', (8, 5))

IBEAM = G.art('''
XXX.XXX
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
...X...
XXX.XXX
''', (3, 8))

QUESTION = G.art('''
.XXXX.
XX..XX
....XX
...XX.
..XX..
..XX..
......
..XX..
''', (0, 0))

PLUS_SMALL = G.art('''
..XX..
..XX..
XXXXXX
XXXXXX
..XX..
..XX..
''', (0, 0))

LINK_SMALL = G.art('''
..XXXX
...XXX
..XXXX
.XXX.X
XXX...
XX....
''', (0, 0))

NO_SMALL = G.fn(8, 8, lambda x, y: 'X' if (
    3.0 <= math.hypot(x - 3.5, y - 3.5) <= 4.1 or
    (abs(x - (7 - y)) <= 0.6 and math.hypot(x - 3.5, y - 3.5) < 4)) else None, (0, 0))

MENU_SMALL = G.art('''
XXXXXXX
XoooooX
XoXXXoX
XoooooX
XoXXXoX
XoooooX
XoXXXoX
XoooooX
XXXXXXX
''', (0, 0))

HOURGLASS = G.art('''
XXXXXXXXXXXXX
XXXXXXXXXXXXX
.XoooooooooX.
.XoooooooooX.
..XoooooooX..
...XoooooX...
....XoooX....
.....XoX.....
.....XoX.....
.....XoX.....
....XoooX....
...XoooooX...
..XoooooooX..
.XoooooooooX.
.XoooooooooX.
XXXXXXXXXXXXX
XXXXXXXXXXXXX
''', (6, 8))

MINI_HOURGLASS = G.art('''
XXXXXXX
.XoooX.
..XoX..
...X...
..XoX..
.XXXXX.
XXXXXXX
''', (0, 0))


def hourglass_frames(n=8):
    """Areia caindo: o bulbo de cima esvazia de cima pra baixo, o de baixo
    enche de baixo pra cima, com um fio de areia no gargalo."""
    top = [(x, y) for y in range(2, 7) for x in range(13) if HOURGLASS.px[y][x] == 'o']
    bot = [(x, y) for y in range(10, 15) for x in range(13) if HOURGLASS.px[y][x] == 'o']
    top.sort(key=lambda p: (-p[1], abs(p[0] - 6)))      # areia assenta no fundo do bulbo
    bot.sort(key=lambda p: (-p[1], abs(p[0] - 6)))
    neck = [(6, 7), (6, 8), (6, 9)]
    cap = min(len(top), len(bot)) * 2 // 3
    frames = []
    for i in range(n):
        p = i / (n - 1)
        g = HOURGLASS.copy()
        for x, y in top[:round(cap * (1 - p))]:
            g.px[y][x] = 'X'
        for x, y in bot[:round(cap * p)]:
            g.px[y][x] = 'X'
        if 0 < p < 1:
            for k, (x, y) in enumerate(neck):
                if (k + i) % 2 == 0:
                    g.px[y][x] = 'X'
        frames.append(g)
    return frames


def crosshair():
    n, c = 19, 9
    def f(x, y):
        if (x == c and (y <= c - 3 or y >= c + 3)) or (y == c and (x <= c - 3 or x >= c + 3)):
            return 'X'
        if x == c and y == c:
            return 'X'
    return G.fn(n, n, f, (c, c))


def bold_plus():
    n, c = 15, 7
    return G.fn(n, n, lambda x, y: 'X' if abs(x - c) <= 1 or abs(y - c) <= 1 else None, (c, c))


def big_x():
    n = 13
    return G.fn(n, n, lambda x, y: 'X' if abs(x - y) <= 1 or abs(x + y - (n - 1)) <= 1 else None,
                (n // 2, n // 2))


def forbidden():
    n = 17
    c = (n - 1) / 2
    def f(x, y):
        r = math.hypot(x - c, y - c)
        if 5.6 <= r <= 8.2:
            return 'X'
        if r < 6 and abs((x - c) + (y - c)) <= 1.0:
            return 'X'
    return G.fn(n, n, f, (n // 2, n // 2))


def head_up(cx, y0, h):
    """Pontas de seta: linha i (a partir da ponta) tem meia-largura i."""
    return [(x, y0 + i) for i in range(h) for x in range(cx - i, cx + i + 1)]


def v_double():
    w, h, c = 11, 19, 5
    g = G(w, h, (c, h // 2))
    for x, y in head_up(c, 0, 6):
        g.px[y][x] = 'X'
    for x, y in head_up(c, 0, 6):
        g.px[h - 1 - (y)][x] = 'X'
    for y in range(6, h - 6):
        for x in (c - 1, c, c + 1):
            g.px[y][x] = 'X'
    return g


def up_arrow():
    w, h, c = 11, 17, 5
    g = G(w, h, (c, 0))
    for x, y in head_up(c, 0, 6):
        g.px[y][x] = 'X'
    for y in range(6, h):
        for x in (c - 1, c, c + 1):
            g.px[y][x] = 'X'
    return g


def diag_double():
    n = 15
    def f(x, y):
        if x + y <= 6 or (n - 1 - x) + (n - 1 - y) <= 6 or abs(x - y) <= 1:
            return 'X'
    return G.fn(n, n, f, (n // 2, n // 2))


def side_n():
    """Borda de cima: barra + seta apontando pra ela."""
    w, c = 15, 7
    g = G(w, 17, (c, 0))
    for y in (0, 1):
        for x in range(w):
            g.px[y][x] = 'X'
    for x, y in head_up(c, 3, 6):
        g.px[y][x] = 'X'
    for y in range(9, 17):
        for x in (c - 1, c, c + 1):
            g.px[y][x] = 'X'
    return g


def corner_nw():
    """Canto de cima à esquerda: cantoneira + seta diagonal apontando pra ela."""
    n = 16
    def f(x, y):
        if (y <= 1 and x <= 12) or (x <= 1 and y <= 12):
            return 'X'
        if x >= 3 and y >= 3 and (x + y <= 12 or (abs(x - y) <= 1 and x + y <= 28)):
            return 'X'
    return G.fn(n, n, f, (0, 0))


def split_v():
    """col-resize: ← ‖ →"""
    w, h, cy = 21, 15, 7
    g = G(w, h, (w // 2, cy))
    for y in range(h):
        for x in (8, 9, 11, 12):
            g.px[y][x] = 'X'
    for i in range(5):  # pontas
        for y in range(cy - i, cy + i + 1):
            g.px[y][i] = 'X'
            g.px[y][w - 1 - i] = 'X'
    for x in range(5, 7):
        for y in (cy - 1, cy, cy + 1):
            g.px[y][x] = 'X'
            g.px[y][w - 1 - x] = 'X'
    return g


def fleur():
    n, c = 21, 10
    g = G(n, n, (c, c))
    for x, y in head_up(c, 0, 5):
        g.px[y][x] = 'X'
        g.px[n - 1 - y][x] = 'X'
        g.px[x][y] = 'X'
        g.px[x][n - 1 - y] = 'X'
    for k in range(5, n - 5):
        for d in (-1, 0, 1):
            g.px[k][c + d] = 'X'
            g.px[c + d][k] = 'X'
    return g


def target():
    n, c = 17, 8
    def f(x, y):
        d = max(abs(x - c), abs(y - c))
        return 'X' if d in (7, 8, 3, 4) or d == 0 else None
    return G.fn(n, n, f, (c, c))


def dot():
    return G.fn(7, 7, lambda x, y: 'X' if math.hypot(x - 3, y - 3) <= 3.2 else None, (3, 3))


def pencil():
    n = 18
    def f(x, y):
        t = x - y               # ao longo do lápis: -17 (ponta) .. +17
        u = x + y - (n - 1)     # perpendicular
        if -16 <= t <= -11 and abs(u) * 2 <= (t + 17) // 1:
            return 'X'          # ponta de grafite
        if -11 < t <= -7 and abs(u) <= 2:
            return 'o'          # madeira
        if -7 < t <= 9 and abs(u) <= 2:
            return 'X'          # corpo
        if 9 < t <= 13 and abs(u) <= 2:
            return 'o'          # borracha
    return G.fn(n, n, f, (0, n - 1))


def zoom(sign):
    n = 17
    c = 6
    def f(x, y):
        r = math.hypot(x - c, y - c)
        if abs(x - y) <= 1 and 20 <= x + y <= 31:
            return 'X'
        if 4.6 <= r <= 6.4:
            return 'X'
        if r < 4.6:
            if (y == c and abs(x - c) <= 2) or (sign > 0 and x == c and abs(y - c) <= 2):
                return 'X'
            return 'o'
    return G.fn(n, n, f, (c, c))


def with_badge(badge, ox=12, oy=12):
    return ARROW.over(badge, ox, oy)


def tee(dir_):
    """top_tee etc. (legado do X11): ⊥ simples."""
    g = G.fn(15, 9, lambda x, y: 'X' if y >= 7 or (x in (6, 7, 8) and y <= 6) else None, (7, 8))
    return g


# ------------------------------------------------------------------ catálogo
# nome principal -> (glifo ou lista de frames, delay ms, [aliases])

def catalog():
    c = {}
    def add(name, glyph, aliases=(), delay=0):
        c[name] = (glyph if isinstance(glyph, list) else [glyph], delay, list(aliases))

    up = up_arrow()
    vd = v_double()
    dd = diag_double()
    sn = side_n()
    cn = corner_nw()
    sv = split_v()

    add('left_ptr', ARROW, ['default', 'arrow', 'top_left_arrow', 'left_arrow',
        'x-cursor', 'X_cursor_default', 'dnd-none', 'dnd-move', 'move-arrow'])
    add('right_ptr', ARROW.flip_h(), ['draft_large', 'draft_small'])
    add('center_ptr', up, ['up_arrow', 'sb_up_arrow'])
    add('sb_down_arrow', up.flip_v(), ['down_arrow'])
    add('sb_left_arrow', up.transpose(), ['left_side_arrow'])
    add('sb_right_arrow', up.transpose().flip_h(), [])
    add('xterm', IBEAM, ['text', 'ibeam'])
    add('vertical-text', IBEAM.transpose(), [])
    add('hand2', HAND, ['pointer', 'hand1', 'hand', 'pointing_hand', 'grab', 'openhand',
        'e29285e634086352946a0e7090d73106', '9d800788f1b08800ae810202380a0822'])
    add('closedhand', FIST, ['grabbing', 'dnd-move-hand', 'fcf21c00b30f7e3f83fe0dfd12e71cff',
        '208530c400c041818281048008011002', '4498f0e0c1937ffe01fd06f973665830',
        '9081237383d90e509aa00f00170e968f'])
    add('watch', hourglass_frames(), ['wait', 'clock'], delay=180)
    add('left_ptr_watch', with_badge(MINI_HOURGLASS), ['progress', 'half-busy',
        '00000000000000020006000e7e9ffc3f', '08e8e1c95fe2fc01f976f1e063a24ccd',
        '3ecb610c1bf2410f44200f48c40d3599'])
    add('question_arrow', with_badge(QUESTION, 12, 11), ['help', 'whats_this', 'left_ptr_help',
        'dnd-ask', '5c6cd98b3f3ebcb1f9c7f1c204630408', 'd9ce0ab605698f320427677b458ad60b'])
    add('context-menu', with_badge(MENU_SMALL, 12, 9), [])
    add('copy', with_badge(PLUS_SMALL), ['dnd-copy', '1081e37283d90000800003c07f3ef6bf',
        '6407b0e94181790501fd1e167b474872', 'b66166c04f8c3109214a4fbd64a50fc8'])
    add('link', with_badge(LINK_SMALL), ['alias', 'dnd-link', '3085a0e285430894940527032f8b26df',
        '640fb0e74195791501fd1ed57b41487f', 'a2a266d0498c3104214a47bd64ab0fc8'])
    add('no-drop', with_badge(NO_SMALL, 12, 12), ['dnd-no-drop'])
    add('crossed_circle', forbidden(), ['not-allowed', 'forbidden', 'circle',
        '03b6e0fcb3499374a867c041f52298f0'])
    add('X_cursor', big_x(), ['pirate', 'kill'])
    add('crosshair', crosshair(), ['cross', 'tcross', 'cross_reverse', 'diamond_cross', 'color-picker'])
    add('plus', bold_plus(), ['cell'])
    add('fleur', fleur(), ['move', 'all-scroll', 'size_all', 'grabbing_all'])
    add('sb_v_double_arrow', vd, ['ns-resize', 'v_double_arrow', 'size_ver', 'n-s-resize',
        'double_arrow', '00008160000006810000408080010102'])
    add('sb_h_double_arrow', vd.transpose(), ['ew-resize', 'h_double_arrow', 'size_hor',
        'e-w-resize', '028006030e0e7ebffc7f7070c0600140'])
    add('fd_double_arrow', dd, ['nwse-resize', 'size_fdiag', 'c7088f0f3e6c8088236ef8e1e3e70000'])
    add('bd_double_arrow', dd.flip_h(), ['nesw-resize', 'size_bdiag', 'fcf1c3c7cd4491d801f1e1c78f100000'])
    add('top_side', sn, ['n-resize'])
    add('bottom_side', sn.flip_v(), ['s-resize'])
    add('left_side', sn.transpose(), ['w-resize'])
    add('right_side', sn.transpose().flip_h(), ['e-resize'])
    add('top_left_corner', cn, ['nw-resize', 'ul_angle'])
    add('top_right_corner', cn.flip_h(), ['ne-resize', 'ur_angle'])
    add('bottom_left_corner', cn.flip_v(), ['sw-resize', 'll_angle'])
    add('bottom_right_corner', cn.flip_h().flip_v(), ['se-resize', 'lr_angle'])
    add('sb_h_double_arrow_split', sv, ['col-resize', 'split_h', '14fef782d02440884392942c11205230'])
    add('sb_v_double_arrow_split', sv.transpose(), ['row-resize', 'split_v', '2870a09082c103050810ffdffffe0204'])
    add('target', target(), ['dotbox', 'dot_box_mask', 'draped_box', 'icon'])
    add('dot', dot(), [])
    add('pencil', pencil(), ['draft'])
    add('zoom-in', zoom(+1), ['zoom_in'])
    add('zoom-out', zoom(-1), ['zoom_out'])
    t = tee('n')
    add('bottom_tee', t, [])
    add('top_tee', t.flip_v(), [])
    add('left_tee', t.transpose().flip_h(), [])
    add('right_tee', t.transpose(), [])
    return c


# ------------------------------------------------------------------ render


def render(g, scale, fill, outline):
    """-> (size, pixels RGBA linhas, hotspot) em canvas quadrado."""
    side = max(BASE, g.w + 3, g.h + 3)
    ox = oy = 1
    solid = lambda x, y: g.get(x, y) not in '._'
    base = [[(0, 0, 0, 0)] * side for _ in range(side)]
    for y in range(side):
        for x in range(side):
            gx, gy = x - ox, y - oy
            c = g.get(gx, gy)
            if c == 'X':
                base[y][x] = fill + (255,)
            elif c == 'o':
                base[y][x] = outline + (255,)
            elif any(solid(gx + dx, gy + dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
                base[y][x] = outline + (255,)
    s = scale
    big = [[base[y // s][x // s] for x in range(side * s)] for y in range(side * s)]
    if SHADOW:
        # sombra suave: a silhueta deslocada ~1 px (em base) pra baixo/direita,
        # com borda desfocada; só aparece onde o cursor é transparente
        n, off = side * s, max(1, s)
        rad = max(1, s // 2)
        sil = [[1 if big[y][x][3] else 0 for x in range(n)] for y in range(n)]
        for y in range(n):
            for x in range(n):
                if big[y][x][3]:
                    continue
                acc = cnt = 0
                for dy in range(-rad, rad + 1):
                    for dx in range(-rad, rad + 1):
                        sy, sx = y - off + dy, x - off + dx
                        cnt += 1
                        if 0 <= sy < n and 0 <= sx < n:
                            acc += sil[sy][sx]
                a = SHADOW * acc // cnt
                if a:
                    big[y][x] = (0, 0, 0, a)
    hx = (g.hot[0] + ox) * s + (s - 1) // 2
    hy = (g.hot[1] + oy) * s + (s - 1) // 2
    return side * s, big, (hx, hy)


def png(size, rows):
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def xcursor(images):
    """images: lista de (nominal, size, rows, (hx,hy), delay)."""
    ntoc = len(images)
    hdr = 16 + 12 * ntoc
    toc, data, pos = b'', b'', hdr
    for nominal, size, rows, (hx, hy), delay in images:
        px = b''.join(struct.pack('<I', (a << 24) | (r << 16) | (gg << 8) | b)
                      for row in rows for (r, gg, b, a) in row)
        chunk = struct.pack('<9I', 36, 0xfffd0002, nominal, 1, size, size, hx, hy, delay) + px
        toc += struct.pack('<3I', 0xfffd0002, nominal, pos)
        data += chunk
        pos += len(chunk)
    return b'Xcur' + struct.pack('<3I', 16, 0x10000, ntoc) + toc + data


def hex_color(s):
    s = s.lstrip('#')
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out', help='diretório do tema a gerar (ex.: build/sgi-red)')
    ap.add_argument('--name', default='sgi-red')
    ap.add_argument('--fill', default='#ff0000', help='cor principal (padrão: vermelho IRIX)')
    ap.add_argument('--outline', default='#ffffff')
    ap.add_argument('--preview', help='grava um PNG com todos os cursores')
    ap.add_argument('--shadow', type=int, default=SHADOW, help='opacidade da sombra 0-255 (0 = sem)')
    a = ap.parse_args()
    globals()['SHADOW'] = a.shadow
    fill, outline = hex_color(a.fill), hex_color(a.outline)

    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    xdir, hdir = os.path.join(a.out, 'cursors'), os.path.join(a.out, 'hyprcursors')
    os.makedirs(xdir)
    os.makedirs(hdir)

    cat = catalog()
    for name, (frames, delay, aliases) in cat.items():
        ximgs = []
        z = io.BytesIO()
        meta = ['resize_algorithm = nearest']
        hot_frac = None
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
            for s in SCALES:
                for i, fr in enumerate(frames):
                    size, rows, hot = render(fr, s, fill, outline)
                    ximgs.append((BASE * s, size, rows, hot, delay or 50))
                    fname = f'{name}_{size}_{i}.png'
                    zf.writestr(fname, png(size, rows))
                    meta.append(f'define_size = {size}, {fname}' + (f', {delay}' if delay else ''))
                    if hot_frac is None:
                        hot_frac = (hot[0] / size, hot[1] / size)
            meta.insert(1, f'hotspot_x = {hot_frac[0]:.4f}')
            meta.insert(2, f'hotspot_y = {hot_frac[1]:.4f}')
            for al in aliases:
                meta.append(f'define_override = {al}')
            zf.writestr('meta.hl', '\n'.join(meta) + '\n')
        with open(os.path.join(hdir, name + '.hlc'), 'wb') as f:
            f.write(z.getvalue())
        with open(os.path.join(xdir, name), 'wb') as f:
            f.write(xcursor(ximgs))
        for al in aliases:
            p = os.path.join(xdir, al)
            if not os.path.lexists(p):
                os.symlink(name, p)

    with open(os.path.join(a.out, 'index.theme'), 'w') as f:
        f.write(f'[Icon Theme]\nName={a.name}\nComment=SGI IRIX style red cursors\nInherits=Adwaita\n')
    with open(os.path.join(a.out, 'manifest.hl'), 'w') as f:
        f.write(f'name = {a.name}\ndescription = SGI IRIX style red cursors\nversion = 1.0\n'
                'cursors_directory = hyprcursors\n')

    if a.preview:
        write_preview(a.preview, cat, fill, outline)
    print(f'{len(cat)} cursores, {sum(len(v[2]) for v in cat.values())} aliases -> {a.out}')


def write_preview(path, cat, fill, outline, s=3, cols=8):
    bg = (0x5f, 0x6f, 0x80, 255)
    cell = 30 * s
    items = [(n, f) for n, (frames, _, _) in cat.items() for f in frames[:1]]
    items += [('watch', f) for f in cat['watch'][0][1:]]
    rows_n = (len(items) + cols - 1) // cols
    W, H = cols * cell, rows_n * cell
    img = [[bg] * W for _ in range(H)]
    for i, (_, g) in enumerate(items):
        size, rows, (hx, hy) = render(g, s, fill, outline)
        bx, by = (i % cols) * cell + (cell - size) // 2, (i // cols) * cell + (cell - size) // 2
        for y in range(size):
            for x in range(size):
                p = rows[y][x]
                if p[3] and 0 <= by + y < H and 0 <= bx + x < W:
                    img[by + y][bx + x] = p
        # marca o hotspot com um pixel ciano
        if 0 <= by + hy < H and 0 <= bx + hx < W:
            img[by + hy][bx + hx] = (0, 255, 255, 255)
    raw = b''.join(b'\0' + bytes(v for p in r for v in p) for r in img)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
