"""
检查生成的字库里到底有没有某些字 —— **解析 cmap，而不是看注释**。

为什么要专门写这个（踩过的坑）：
    一开始用 `/\* *U\+XXXX` 去匹配文件里的注释行，以为那就是字库内容。
    但注释列的是**字形位图**，而 LVGL 真正查表用的是 `cmaps[]` +
    `unicode_list_N[]`（SPARSE_TINY 格式，存的是**相对 range_start 的偏移**）。
    两者不是一回事，只读注释会得出错误结论 —— 我就因此误判过一次，
    以为字库没问题，结果屏幕上是一片豆腐块。

    所以本脚本按**结构**解析：
      1. 取出 cmaps[] 的每一项，拿到 range_start 和它引用的 unicode_list_N
      2. 按名字取到对应的 unicode_list_N[] 的值
      3. 码点 = range_start + 值
      4. 再加上 FORMAT0_TINY 那段的连续区间（ASCII）

用法:
    python tools/check_glyphs.py 触摸手表
    python tools/check_glyphs.py --all      # 检查 watch_strings.h 里所有中文
"""
import re
import sys
import pathlib

FW = pathlib.Path(__file__).resolve().parent.parent
FONT_DIR = FW / "components" / "fonts"
STRINGS = FW / "components" / "watch_ui_pages" / "include" / "watch_strings.h"


def strip_comments(src):
    """去掉 /* */ 和 // 注释，避免注释里的数字干扰解析。"""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def font_coverage(path):
    """返回该字体真正能查到的码点集合。"""
    src = strip_comments(path.read_text(encoding="utf-8"))
    covered = set()

    # --- 取 unicode_list_N 数组内容 ---
    lists = {}
    for m in re.finditer(
        r"unicode_list_(\d+)\s*\[\s*\]\s*=\s*\{(.*?)\}", src, re.S
    ):
        idx = int(m.group(1))
        lists[idx] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", m.group(2))]

    # --- 逐个 cmap 项处理 ---
    m = re.search(r"cmaps\s*\[\s*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not m:
        raise SystemExit(f"在 {path} 里找不到 cmaps[]")

    for item in re.finditer(r"\{(.*?)\}", m.group(1), re.S):
        body = item.group(1)

        def num(key):
            mm = re.search(key + r"\s*=\s*(\d+)", body)
            return int(mm.group(1)) if mm else None

        start = num(r"\.range_start")
        if start is None:
            continue

        ctype = re.search(r"\.type\s*=\s*(LV_FONT_FMT_TXT_CMAP_\w+)", body)
        ctype = ctype.group(1) if ctype else ""

        ref = re.search(r"\.unicode_list\s*=\s*unicode_list_(\d+)", body)
        if ref:
            offs = lists.get(int(ref.group(1)), [])
            # SPARSE：值是相对 range_start 的偏移
            for off in offs:
                covered.add(start + off)
        else:
            # FORMAT0：连续区间
            length = num(r"\.range_length") or 0
            covered.update(range(start, start + length))

    return covered


def main():
    check_all = "--all" in sys.argv
    texts = [a for a in sys.argv[1:] if not a.startswith("--")]

    if check_all:
        s = STRINGS.read_text(encoding="utf-8")
        texts = ["".join(sorted({c for c in s if 0x4E00 <= ord(c) <= 0x9FFF}))]

    if not texts:
        print(__doc__)
        return 1

    ok = True
    for size in (14, 16, 20):
        path = FONT_DIR / f"lv_font_watch_{size}.c"
        covered = font_coverage(path)
        cjk = {c for c in covered if 0x4E00 <= c <= 0x9FFF}
        print(f"=== {size}px  覆盖 {len(covered)} 个码点（其中汉字 {len(cjk)}）===")

        for t in texts:
            miss = [c for c in t if c != " " and ord(c) not in covered]
            if miss:
                print(f"    缺 {len(miss)} 个: {''.join(miss)}")
                ok = False
            else:
                print(f"    全部覆盖 ✓")
        print()

    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
