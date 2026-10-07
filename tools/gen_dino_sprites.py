#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把参考工程里的 Chrome 恐龙精灵表，转成本项目要用的 2bpp 位图 C 数组。

## 为什么要写这个工具（而不是手工抄点阵）

参考表是 **2441x130 的 8 位调色板 PNG，内容就是 Chrome 原版离线精灵表的 2 倍图**
（恐龙 88x94、小仙人掌 30x66、大仙人掌 46x92、翼龙、云、1200x7 的地面条）。
手工抄点阵不可能，所以做成"给一个种子坐标 -> 自动找出该精灵的精确包围盒 -> 折半 -> 打包"。

## 两个必须知道的坑

1. **不能用 alpha 判形状**。
   表里的精灵是**画在不透明白色卡片上**的（alpha 全是 255），
   所以"不透明"这个条件会把整张白卡片当成精灵。
   必须按**亮度**取墨色：`opaque and luma < thr`。
   云朵是浅灰（luma≈218），阈值要单独放宽到 230，否则整朵云都不见了。

2. **必须按 2:1 隔点取样折半**，不能缩放。
   表是精确 2 倍图，隔点取正好得到原版 1x 像素，一个像素都不会糊。
   一旦改成双线性缩放，像素画会立刻变毛边。

## 用法

    python tools/gen_dino_sprites.py                 # 生成 C 文件
    python tools/gen_dino_sprites.py --ascii         # 顺便打印每个精灵的点阵（肉眼核对）
    python tools/gen_dino_sprites.py --montage m.png # 顺便拼一张预览图

## 版权

精灵图来自 Chromium 的离线小恐龙（BSD-3-Clause），
参考工程 https://github.com/ockhamrazor/chrome-dinosaur-game （ISC）。
生成物头部会写明来源；本工程自用没问题，对外发布要保留这段声明。

⚠️ 参考工程放在 `相关资料/`，**不在 git 仓库里**。
   所以：生成物 `.c` 提交进仓库，构建永远不依赖那个目录；
   这个脚本找不到源图时会明确报错，而不是生成一张空表。
"""
import argparse
import os
import re
import struct
import sys
import zlib
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)                      # firmware/smartwatch
PROJ = os.path.dirname(os.path.dirname(FW))     # SmartWatch

DEFAULT_SHEET_CANDIDATES = [
    os.path.join(PROJ, "相关资料", "示例工程", "chrome-dinosaur-game", "image", "sprite01.png"),
    os.path.join(PROJ, "相关资料", "示例工程", "chrome-dinosaur-game", "image", "sprite02.png"),
]

OUT_C = os.path.join(FW, "components", "watch_ui_pages", "games", "dino_sprites.c")
OUT_H = os.path.join(FW, "components", "watch_ui_pages", "games", "dino_sprites.h")

# ==================================================================
# 精灵表
#
#   mode = "comp"        给种子点，自动取"包含该点的那一团墨"的包围盒
#   mode = "cell"        给固定矩形（地面条那种超长条用它）
#   mode = "comp_center" 取包围盒中心，再按固定尺寸裁（翼龙两帧要对齐身体）
#
# 坐标是**源图 2x 坐标**，阈值 thr 见上面坑 1 的说明。
# ink: 1 = 前景（恐龙/仙人掌/翼龙）  2 = 次要（地面/云）
# ==================================================================
SPRITES = [
    # ---- 恐龙：7 帧，全部 80x86@2x -> 40x43@1x ----
    # 站立（左边那只，独立于右侧 6 帧）
    ("dino_idle",   "comp",        (116, 40),  160, 1),
    # 右侧 6 帧：跑 x3（圆眼）、死亡 x3（方眼）。包围盒完全一致，所以动画不会抖。
    ("dino_run1",   "comp",        (1720, 40), 160, 1),
    ("dino_run2",   "comp",        (1808, 40), 160, 1),
    ("dino_jump",   "comp",        (1896, 40), 160, 1),
    ("dino_dead",   "comp",        (2072, 40), 160, 1),

    # ---- 仙人掌：小号 30x66@2x -> 15x33，大号 46x92@2x -> 23x46 ----
    ("cactus_small1", "comp",      (497, 40),  160, 1),
    ("cactus_small2", "comp",      (565, 40),  160, 1),
    ("cactus_large1", "comp",      (677, 40),  160, 1),
    ("cactus_large2", "comp",      (777, 40),  160, 1),

    # ---- 翼龙：两帧翅膀（上/下） ----
    # 两帧的墨迹中心相差 16px@2x（=8px@1x），所以必须**按包围盒中心对齐**，
    # 否则扇翅膀时整只鸟会上下跳。框取 92x64@2x -> 46x32@1x：
    # 再高一点（72）上帧的框会算出 y=-4 —— 负坐标在 Python 里会静默绕到
    # 图片末尾读数据，生成一堆垃圾点阵且不报错。下面的越界断言就是防这个的。
    ("bird1",       "comp_center", (306, 48, 92, 64), 160, 1),
    ("bird2",       "comp_center", (398, 32, 92, 64), 160, 1),

    # ---- 云：浅灰，阈值必须放宽 ----
    ("cloud",       "cell",        (174, 2, 84, 28),  230, 2),

    # ---- 地面：把主线和下面的小石子一起带上（1200x10@1x） ----
    ("ground",      "cell",        (2, 104, 2400, 20), 160, 2),
]


# ==================================================================
# 一、极简 PNG 解码（只用标准库；支持调色板/灰度/RGB/RGBA，1/2/4/8 位）
# ==================================================================
def png_read(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"不是 PNG：{path}")

    pos, idat, plte, trns, ihdr = 8, b"", None, None, None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln

    if ihdr is None:
        raise ValueError("PNG 缺 IHDR")
    w, h, bitd, ctype, _, _, interlace = ihdr
    if interlace:
        raise ValueError("不支持隔行 PNG")

    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = (w * ch * bitd + 7) // 8
    raw = zlib.decompress(idat)

    # 反滤波
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line

    # 展开成 (r,g,b,a) 二维表
    pal = []
    if plte:
        for i in range(len(plte) // 3):
            a = trns[i] if (trns and i < len(trns)) else 255
            pal.append((plte[3 * i], plte[3 * i + 1], plte[3 * i + 2], a))

    px = []
    for y in range(h):
        base = y * stride
        row = []
        for x in range(w):
            if bitd == 8:
                o = base + x * ch
                if ctype == 3:
                    row.append(pal[out[o]])
                elif ctype == 0:
                    v = out[o]
                    row.append((v, v, v, 255))
                elif ctype == 2:
                    row.append((out[o], out[o + 1], out[o + 2], 255))
                elif ctype == 4:
                    v = out[o]
                    row.append((v, v, v, out[o + 1]))
                else:
                    row.append((out[o], out[o + 1], out[o + 2], out[o + 3]))
            else:
                if bitd == 1:
                    bit = (out[base + (x >> 3)] >> (7 - (x & 7))) & 1
                    idx = bit
                elif bitd == 2:
                    idx = (out[base + (x >> 2)] >> (6 - 2 * (x & 3))) & 3
                elif bitd == 4:
                    b = out[base + (x >> 1)]
                    idx = (b >> 4) if (x & 1) == 0 else (b & 0xF)
                else:
                    raise ValueError(f"不支持的位深 {bitd}")
                if ctype == 3:
                    row.append(pal[idx])
                else:
                    v = idx * 255 // ((1 << bitd) - 1)
                    row.append((v, v, v, 255))
        px.append(row)
    return w, h, px


# ==================================================================
# 二、取墨 / 找连通块
# ==================================================================
def ink_mask(px, thr):
    """墨 = 不透明 且 亮度 < thr。**不能只看 alpha**（见文件头坑 1）。"""
    h, w = len(px), len(px[0])
    mask = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[y][x]
            if a > 128 and (r * 299 + g * 587 + b * 114) // 1000 < thr:
                mask[y][x] = True
    return mask


def component_find(mask, seed):
    """找出包含 seed 的那一团墨。

    返回 (bbox, members)：
      bbox    = (x, y, w, h)
      members = 与 mask 同尺寸的布尔表，只有**这一团**的像素为 True

    ⚠️ 为什么必须把 members 一起返回（踩过）：
       包围盒是个矩形，矩形里可能还有**别的**墨团 ——
       站立恐龙脚下就压着地面的小石子和横线，它们和恐龙不相连但落在同一个矩形里。
       只按矩形裁剪的话，这些石子会被当成恐龙的一部分打进点阵，
       游戏里恐龙脚边就飘着两颗黑点（放大看才发现）。
       所以裁剪要按 members 取，不能按 mask 取。
    """
    h, w = len(mask), len(mask[0])
    sx, sy = seed
    if not (0 <= sx < w and 0 <= sy < h) or not mask[sy][sx]:
        raise ValueError(f"种子点 {seed} 不是墨（坐标错或阈值不对）")
    members = [[False] * w for _ in range(h)]
    q = deque([(sx, sy)])
    members[sy][sx] = True
    x0 = x1 = sx
    y0 = y1 = sy
    while q:
        cx, cy = q.popleft()
        x0, x1 = min(x0, cx), max(x1, cx)
        y0, y1 = min(y0, cy), max(y1, cy)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                nx, ny = cx + dx, cy + dy
                if 0 <= nx < w and 0 <= ny < h and mask[ny][nx] and not members[ny][nx]:
                    members[ny][nx] = True
                    q.append((nx, ny))
    return (x0, y0, x1 - x0 + 1, y1 - y0 + 1), members


def resolve_rect(mode, mask, arg, img_w, img_h):
    """-> (rect, members)；members 为 None 表示"整个矩形都算"（cell 模式）。"""
    members = None
    if mode == "cell":
        rect = tuple(arg)
    elif mode == "comp":
        rect, members = component_find(mask, arg)
    elif mode == "comp_center":
        cx_seed, cy_seed, cw, ch = arg
        (x, y, w, h), members = component_find(mask, (cx_seed, cy_seed))
        ccx, ccy = x + w // 2, y + h // 2
        rect = (ccx - cw // 2, ccy - ch // 2, cw, ch)
    else:
        raise ValueError(f"未知 mode {mode}")

    # ⚠️ 越界必须**当错误报出来**，不能让负坐标绕到内存末尾去读 ——
    #    那样会安安静静生成一张垃圾点阵，编译还照样通过。
    x0, y0, w, h = rect
    if x0 < 0 or y0 < 0 or x0 + w > img_w or y0 + h > img_h:
        raise ValueError(
            f"裁剪框 {rect} 超出源图 {img_w}x{img_h}（负坐标/越界会让取样读到错的数据）")
    return rect, members


# ==================================================================
# 三、2:1 隔点折半 + 2bpp 打包
# ==================================================================
def extract(mask, rect, ink, members=None):
    """隔点取样得到 1x 的 2bpp 位图。返回 (w, h, stride, bytes)

    members 非空时只认这一团的像素（丢掉矩形里的其他墨团，见 component_find）。
    """
    x0, y0, w2, h2 = rect
    w, h = w2 // 2, h2 // 2
    stride = (w * 2 + 7) // 8
    buf = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            sx, sy = x0 + x * 2, y0 + y * 2
            hit = members[sy][sx] if members is not None else mask[sy][sx]
            if hit:
                buf[y * stride + (x * 2) // 8] |= ink << (6 - 2 * (x & 3))
    return w, h, stride, bytes(buf)


def to_ascii(w, h, stride, bits):
    rows = []
    for y in range(h):
        line = []
        for x in range(w):
            v = (bits[y * stride + (x * 2) // 8] >> (6 - 2 * (x & 3))) & 3
            line.append("#" if v else ".")
        rows.append("".join(line))
    return rows


# ==================================================================
# 四、输出
# ==================================================================
def emit_c(sprites, sheet_note):
    lines = []
    a = lines.append
    a("/**")
    a(" * @file dino_sprites.c")
    a(" * @brief 小恐龙游戏的精灵位图 —— **本文件由脚本生成，不要手改**")
    a(" *")
    a(" * 生成命令：")
    a(" *     python tools/gen_dino_sprites.py")
    a(" * 生成脚本：tools/gen_dino_sprites.py（里面写了完整的坑和解法）")
    a(" *")
    a(f" * 源图：{sheet_note}")
    a(" * 素材版权：Chromium 离线小恐龙（BSD-3-Clause）")
    a(" *           参考工程 https://github.com/ockhamrazor/chrome-dinosaur-game （ISC）")
    a(" *")
    a(" * 存储格式：2bpp 调色板索引，行按字节对齐（stride = (w*2+7)/8）")
    a(" *           0 = 透明   1 = 前景墨色   2 = 次要墨色")
    a(" *")
    a(" * 为什么是 2bpp 而不是 1bpp：地面和云要用浅灰，恐龙/仙人掌用深色，")
    a(" * 一共 3 种状态（含透明），1bpp 不够；4 色共 16.8KB 画布，内部 RAM 装得下。")
    a(" */")
    a('#include "dino_sprites.h"')
    a("")
    for s in sprites:
        a("/*")
        a(f" * {s['name']}  {s['w']}x{s['h']} (1x)  行字节数 {s['stride']}  墨色 {s['ink']}")
        a(" *")
        for row in s["rows"]:
            a(f" *   {row}")
        a(" */")
        a(f"static const uint8_t bits_{s['name']}[] = {{")
        b = s["bits"]
        for i in range(0, len(b), 16):
            chunk = ", ".join(f"0x{v:02X}" for v in b[i:i + 16])
            a(f"    {chunk},")
        a("};")
        a("")
    for s in sprites:
        a(f"const w_gfx_sprite_t dino_spr_{s['name']} = "
          f"{{ {s['w']}, {s['h']}, {s['stride']}, {s['ink']}, bits_{s['name']} }};")
    a("")
    return "\n".join(lines)


def emit_h(sprites):
    lines = []
    a = lines.append
    a("/**")
    a(" * @file dino_sprites.h")
    a(" * @brief 小恐龙精灵位图表 —— **本文件由脚本生成，不要手改**")
    a(" *")
    a(" * 生成命令：python tools/gen_dino_sprites.py")
    a(" *")
    a(" * 精灵类型用的是 watch_ui 里的公共定义 `w_gfx_sprite_t`（watch_gfx.h），")
    a(" * 不在这里另定义一个 —— 否则贴图函数收到的是「长得一样但类型不同」的")
    a(" * 指针：C 只给个警告就放过去了，字段顺序一旦不一致就是花屏。")
    a(" */")
    a("#pragma once")
    a("")
    a("#include <stdint.h>")
    a('#include "watch_gfx.h"      /* w_gfx_sprite_t */')
    a("")
    a("#ifdef __cplusplus")
    a('extern "C" {')
    a("#endif")
    a("")
    for s in sprites:
        a(f"extern const w_gfx_sprite_t dino_spr_{s['name']};"
          f"   /* {s['w']}x{s['h']} */")
    a("")
    a("#ifdef __cplusplus")
    a("}")
    a("#endif")
    a("")
    return "\n".join(lines)


def write_montage(path, sprites, zoom=2, gap=4):
    """把生成出来的精灵竖着拼成一张图，方便肉眼核对（不是构建产物）。"""
    W = max(s["w"] for s in sprites) * zoom + 8
    H = sum(s["h"] * zoom + gap for s in sprites) + gap
    img = bytearray(b"\xff" * (W * H * 3))          # 白底

    def put(x, y, v):
        o = (y * W + x) * 3
        img[o] = img[o + 1] = img[o + 2] = v

    yy = gap
    for s in sprites:
        for y in range(s["h"]):
            for x in range(s["w"]):
                v = (s["bits"][y * s["stride"] + (x * 2) // 8] >> (6 - 2 * (x & 3))) & 3
                if not v:
                    continue
                shade = 0 if v == 1 else 150
                for dy in range(zoom):
                    for dx in range(zoom):
                        put(4 + x * zoom + dx, yy + y * zoom + dy, shade)
        yy += s["h"] * zoom + gap

    raw = b"".join(b"\x00" + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d
                + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)
    return W, H


def write_review(path, sprites, zoom=3, gap=8, ground_slice=240):
    """人眼核对用的排版图：恐龙一行、障碍一行、地面截一段。

    和 --montage 的区别：那张是"原样竖排"（能看出真实尺寸），
    这张是"放大分组"（能看清形状，专门用来确认没裁错精灵）。
    """
    dinos = [s for s in sprites if s["name"].startswith("dino_")]
    obs = [s for s in sprites if s["name"].startswith(("cactus_", "bird"))]
    other = [s for s in sprites if s not in dinos and s not in obs]

    def draw_w(s):
        # 地面条太长，只画前面一段（整条 1200px 画进来会把图撑爆）
        return min(s["w"], ground_slice) if s["name"] == "ground" else s["w"]

    def row_size(row):
        w = sum(draw_w(s) * zoom + gap for s in row) + gap
        h = max((s["h"] for s in row), default=0) * zoom + 2 * gap
        return w, h

    rows = [dinos, obs]
    for s in other:
        rows.append([s])
    W = max(row_size(r)[0] for r in rows)
    H = sum(row_size(r)[1] for r in rows)
    img = bytearray(b"\xff" * (W * H * 3))

    def put(x, y, v):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            img[o] = img[o + 1] = img[o + 2] = v

    yy = 0
    for row in rows:
        rh = row_size(row)[1]
        xx = gap
        for s in row:
            sw = draw_w(s)
            base = (rh - 2 * gap - s["h"] * zoom) // 2 + gap      # 行内垂直居中
            for y in range(s["h"]):
                for x in range(sw):
                    v = (s["bits"][y * s["stride"] + (x * 2) // 8] >> (6 - 2 * (x & 3))) & 3
                    if not v:
                        continue
                    shade = 0 if v == 1 else 150
                    for dy in range(zoom):
                        for dx in range(zoom):
                            put(xx + x * zoom + dx, yy + base + y * zoom + dy, shade)
            xx += sw * zoom + gap
        yy += rh

    raw = b"".join(b"\x00" + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d
                + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)
    return W, H


# ==================================================================
def main():
    ap = argparse.ArgumentParser(description="生成小恐龙精灵位图 C 数组")
    ap.add_argument("--sheet", help="参考精灵表 PNG 路径（默认自动找 相关资料/ 下的那张）")
    ap.add_argument("--ascii", action="store_true", help="打印每个精灵的点阵")
    ap.add_argument("--montage", help="额外输出一张预览 PNG（原样竖排，看真实尺寸）")
    ap.add_argument("--review", help="额外输出一张放大分组图（看形状对不对）")
    ap.add_argument("--dry-run", action="store_true", help="只解析不写文件")
    args = ap.parse_args()

    sheet = args.sheet
    if not sheet:
        for cand in DEFAULT_SHEET_CANDIDATES:
            if os.path.exists(cand):
                sheet = cand
                break
    if not sheet or not os.path.exists(sheet):
        print("找不到参考精灵表。它不在 git 仓库里（在 相关资料/ 下），请用 --sheet 指定：\n"
              "  python tools/gen_dino_sprites.py --sheet "
              "\"../../相关资料/示例工程/chrome-dinosaur-game/image/sprite01.png\"",
              file=sys.stderr)
        return 2

    print(f"源图: {sheet}")
    sw, sh, px = png_read(sheet)
    print(f"      {sw}x{sh}")

    masks = {}
    sprites = []
    for (name, mode, arg, thr, ink) in SPRITES:
        if thr not in masks:
            masks[thr] = ink_mask(px, thr)
        mask = masks[thr]
        rect, members = resolve_rect(mode, mask, arg, sw, sh)
        w, h, stride, bits = extract(mask, rect, ink, members)
        rows = to_ascii(w, h, stride, bits)
        n_ink = sum(r.count("#") for r in rows)
        sprites.append(dict(name=name, w=w, h=h, stride=stride, ink=ink,
                            bits=bits, rows=rows, rect=rect))
        print(f"  {name:15s} rect2x=({rect[0]:4d},{rect[1]:3d},{rect[2]:4d},{rect[3]:3d})"
              f"  -> 1x {w:3d}x{h:<3d} stride={stride:3d} 墨点={n_ink}")

        if n_ink == 0:
            print(f"  ✗ {name} 是空的！阈值/种子点不对", file=sys.stderr)
            return 3

    if args.ascii:
        for s in sprites:
            print(f"\n===== {s['name']} {s['w']}x{s['h']} =====")
            for row in s["rows"]:
                print("  " + row)

    if args.montage:
        W, H = write_montage(args.montage, sprites)
        print(f"预览图: {args.montage} ({W}x{H})")
    if args.review:
        W, H = write_review(args.review, sprites)
        print(f"核对图: {args.review} ({W}x{H})")

    if args.dry_run:
        print("--dry-run：没有写文件")
        return 0

    names = ", ".join(sorted(set(s["name"] for s in sprites)))
    note = os.path.relpath(sheet, PROJ).replace("\\", "/")
    open(OUT_C, "w", encoding="utf-8", newline="\n").write(emit_c(sprites, note))
    open(OUT_H, "w", encoding="utf-8", newline="\n").write(emit_h(sprites))
    print(f"\n已写入:\n  {os.path.relpath(OUT_C, FW)}\n  {os.path.relpath(OUT_H, FW)}")
    print(f"精灵 {len(sprites)} 个: {names}")
    print(f"位图总计 {sum(len(s['bits']) for s in sprites)} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
