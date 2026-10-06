"""
检查**所有界面字符串字面量**里的中文是否都在字库里。

和 check_glyphs.py 的区别：
    check_glyphs.py 只检查 watch_strings.h 里的宏；
    本脚本扫全部 .c/.h 的**字符串字面量**，更彻底 ——
    能抓出"写在页面代码里的中文字面量漏进字库"这种问题
    （日期格式串 "%d年%d月%d日" 就是这么漏的）。

用法:
    python tools/check_all_literals.py
"""
import re
import sys
import pathlib

FW = pathlib.Path(__file__).resolve().parent.parent
FONT_DIR = FW / "components" / "fonts"
SCAN_DIRS = ["watch_ui_pages", "watch_ui", "app"]

CJK = re.compile(r"[\u4E00-\u9FFF\u3000-\u303F\uFF00-\uFFEF]")
LITERAL = re.compile(r'"([^"\\\r\n]|\\.)*"')


def font_coverage(path):
    src = re.sub(r"/\*.*?\*/", " ", path.read_text(encoding="utf-8"), flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)

    lists = {}
    for m in re.finditer(r"unicode_list_(\d+)\s*\[\s*\]\s*=\s*\{(.*?)\}", src, re.S):
        lists[int(m.group(1))] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]+)", m.group(2))]

    covered = set()
    cm = re.search(r"cmaps\s*\[\s*\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not cm:
        raise SystemExit(f"找不到 cmaps[]: {path}")

    for item in re.finditer(r"\{(.*?)\}", cm.group(1), re.S):
        body = item.group(1)
        m_start = re.search(r"\.range_start\s*=\s*(\d+)", body)
        if not m_start:
            continue
        start = int(m_start.group(1))
        ref = re.search(r"\.unicode_list\s*=\s*unicode_list_(\d+)", body)
        if ref:
            for off in lists.get(int(ref.group(1)), []):
                covered.add(start + off)
        else:
            m_len = re.search(r"\.range_length\s*=\s*(\d+)", body)
            if m_len:
                covered.update(range(start, start + int(m_len.group(1))))
    return covered


def main():
    have = font_coverage(FONT_DIR / "lv_font_watch_16.c")

    # 收集所有字面量里出现的非 ASCII 字符
    used = {}
    for d in SCAN_DIRS:
        root = FW / "components" / d
        if not root.exists():
            continue
        for path in root.rglob("*.[ch]"):
            text = path.read_text(encoding="utf-8")
            # 去掉注释，免得把注释里的中文也算进来
            text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
            text = re.sub(r"//[^\n]*", " ", text)
            for lit in LITERAL.finditer(text):
                for ch in lit.group(0):
                    if ord(ch) > 0x7F:
                        used.setdefault(ch, set()).add(path.name)

    missing = {c: f for c, f in used.items() if ord(c) not in have}

    print(f"字面量里用到的非 ASCII 字符：{len(used)} 个")
    print(f"字库覆盖：{len(have)} 个码点")
    if not missing:
        print("✓ 全部覆盖，不会出现豆腐块")
        return 0

    print(f"\n✗ 有 {len(missing)} 个字符不在字库里（会显示成豆腐块）：\n")
    for ch in sorted(missing):
        files = ", ".join(sorted(missing[ch]))
        print(f"    {ch}  U+{ord(ch):04X}   出现在: {files}")
    print("\n处理办法：把它们写进 components/watch_ui_pages/include/watch_strings.h，")
    print("          然后重跑 tools/gen_font.ps1")
    return 2


if __name__ == "__main__":
    sys.exit(main())
