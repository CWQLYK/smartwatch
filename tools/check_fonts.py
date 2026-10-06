"""
静态检查：找出手表界面代码里"用了纯 ASCII 字体、却可能显示中文"的标签。

为什么要这个检查（踩过的坑，踩了两次）：
    中文字库是按需生成的（components/fonts），只有设成 watch_font_cn(n)
    的标签才用得到。如果一个标签设成 W_FONT_SMALL（LVGL 自带的 Montserrat，
    只有 ASCII），而文案是中文，LVGL 查不到字形就画**豆腐块** ——
    不报错、不崩溃，只是难看，最容易漏。

    踩过的两次：
      1. watch_page_home.c 里 "%d年%d月%d日" 的年月没进字库
      2. watch_page_settings.c 右侧值列用了 W_FONT_SMALL，但值是中文

检查原理：
    在文件里维护一张"变量 → 文案"的表（扫描 xxx = "字符串" / 宏 / 局部变量），
    再看每个 w_label_create / w_metric_card_create 的第 3 个参数用的是什么字体。
    变量解析不出来时**按最坏情况处理**（当成可能是中文），宁可误报。

用法:
    python tools/check_fonts.py
"""
import re
import sys
import pathlib

FW = pathlib.Path(__file__).resolve().parent.parent
PAGES = FW / "components" / "watch_ui_pages"
UI = FW / "components" / "watch_ui"
STRINGS = PAGES / "include" / "watch_strings.h"

ASCII_FONTS = {"W_FONT_TINY", "W_FONT_SMALL", "W_FONT_NORMAL", "W_FONT_MEDIUM",
               "W_FONT_LARGE", "W_FONT_HUGE", "W_FONT_CLOCK"}

CJK = re.compile(r"[\u4E00-\u9FFF\u3000-\u303F\uFF00-\uFFEF]")

# 这些控件工厂的第 3 个参数是"字体"
LABEL_FACTORIES = ("w_label_create",)


def load_macros():
    src = STRINGS.read_text(encoding="utf-8")
    macros = {}
    for m in re.finditer(r'#define\s+(W_STR_\w+)\s+"([^"]*)"', src):
        macros[m.group(1)] = m.group(2)
    return macros


def expand(text, macros):
    out = text
    for _ in range(6):
        before = out
        for name, val in macros.items():
            out = out.replace(name, val)
        if out == before:
            break
    return out


def split_args(argstr):
    """按逗号切参数，跳过括号/引号内部。"""
    args, depth, cur, in_str = [], 0, "", False
    i = 0
    while i < len(argstr):
        c = argstr[i]
        if in_str:
            cur += c
            if c == '"' and argstr[i - 1] != "\\":
                in_str = False
        elif c == '"':
            in_str = True
            cur += c
        elif c in "([{":
            depth += 1
            cur += c
        elif c in ")]}":
            depth -= 1
            cur += c
        elif c == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        args.append(cur.strip())
    return args


def collect_string_vars(lines, macros):
    """
    扫一遍文件，收集"变量 → 文案"映射。
    只认简单形式：name = "..."  /  name = W_STR_XXX  /  name = 另一个变量
    """
    varmap = {}
    for line in lines:
        # 去掉行尾注释
        line = re.sub(r"//.*$", "", line)
        m = re.search(r"\b(\w+)\s*=\s*(.+?);", line)
        if not m:
            continue
        name, rhs = m.group(1), m.group(2).strip()
        # 字符串字面量（可能有多个拼接）
        lits = re.findall(r'"([^"]*)"', rhs)
        if lits:
            varmap[name] = "".join(lits)
        elif re.fullmatch(r"\w+", rhs):
            varmap[name] = varmap.get(rhs, "")       # 变量传递
        else:
            # 表达式（函数调用等）→ 不知道，留空
            varmap.setdefault(name, "")
    return varmap


def looks_chinese(expr, macros, varmap):
    """
    判断这个参数表达式是否"可能含中文"。
    解析不出来时返回 True（宁可误报，也不要漏掉豆腐块）。
    """
    expanded = expand(expr, macros)
    if CJK.search(expanded):
        return True, "字面量含中文"

    e = expr.strip()
    # 纯字符串字面量且无中文 → 安全
    if re.fullmatch(r'"[^"]*"(\s*"[^"]*")*', e):
        return False, ""
    # 变量 → 查表
    if re.fullmatch(r"\w+", e):
        if e in varmap:
            v = expand(varmap[e], macros)
            return (CJK.search(v) is not None), f"变量 {e} = {varmap[e]!r}"
        return True, f"变量 {e} 解析不出来（按最坏情况处理）"
    # 其他表达式（拼接、函数返回）→ 保守
    return True, "表达式无法静态判断（按最坏情况处理）"


def main():
    macros = load_macros()
    problems = []

    for path in sorted(list(PAGES.rglob("*.c")) + list(UI.rglob("*.c"))):
        text = path.read_text(encoding="utf-8")
        lines = text.splitlines()
        varmap = collect_string_vars(lines, macros)

        for ln, line in enumerate(lines, 1):
            if not any(f in line for f in LABEL_FACTORIES):
                continue
            # 可能跨行，拼一个 3 行的窗口
            chunk = " ".join(lines[ln - 1: ln + 2])
            m = re.search(r"w_label_create\s*\((.*)", chunk, re.S)
            if not m:
                continue
            args = split_args(m.group(1))
            if len(args) < 4:
                continue
            textarg, fontarg = args[1], args[2]

            bad_font = [f for f in ASCII_FONTS if re.fullmatch(rf"{f}", fontarg.strip())]
            if not bad_font:
                continue

            chinese, why = looks_chinese(textarg, macros, varmap)
            if chinese:
                problems.append((path, ln, bad_font[0], textarg, why))

    if not problems:
        print("✓ 没发现「ASCII 字体 + 可能含中文的文案」的组合")
        return 0

    print("✗ 以下标签用了纯 ASCII 字体，但文案可能含中文 → 屏幕上会是豆腐块：\n")
    for path, ln, font, textarg, why in problems:
        rel = path.relative_to(FW)
        print(f"  {rel}:{ln}   [{font}]")
        print(f"      文案: {textarg}")
        print(f"      依据: {why}")
        print(f"      → 改成 watch_font_cn(14) / watch_font_cn(16)\n")
    return 2


if __name__ == "__main__":
    sys.exit(main())
