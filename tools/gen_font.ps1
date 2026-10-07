<#
.SYNOPSIS
    生成中文字库（lv_font_conv 封装）—— 自动收集所有用到的汉字

.DESCRIPTION
    为什么需要这个脚本：
      LVGL 自带字体不含中文，而全量 CJK 字库有几十 MB，Flash 装不下。
      所以必须"按需抽字"。手工维护码点列表迟早会漏字，症状还特别迷惑：
      字形查得到、宽度正常，但屏幕上是豆腐块。

      本脚本从 components/watch_ui_pages/include/watch_strings.h
      **自动**扫出所有汉字（那是全项目唯一写中文的地方），
      再调 lv_font_conv 抽出 14/16/20 三档点阵字体。

    项目里的"中文集中管理 → 一键生成字库 → 零豆腐块"闭环就是它。

.PARAMETER Size
    只生成指定字号（默认生成 14 / 16 / 20 全部）

.PARAMETER Force
    即使字体文件已存在也重新生成

.EXAMPLE
    .\gen_font.ps1
    # 扫描 watch_strings.h，生成三个 .c 到 components/fonts/

.EXAMPLE
    .\gen_font.ps1 -Size 16
    # 只重新生成 16px

.NOTES
    ⚠️ 必须加 --no-compress —— LVGL 8.x 读不了 lv_font_conv 的压缩位图。
       去掉它不会报错，只会安静地给你一屏豆腐块。
#>
[CmdletBinding()]
param(
    [ValidateSet(14, 16, 20)]
    [int[]]$Size = @(14, 16, 20),

    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ---------- 路径 ----------
$ScriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir                       # firmware/smartwatch
$CompDir     = Join-Path $ProjectRoot "components"
$OutDir      = Join-Path $CompDir "fonts"

# ---------- lv_font_conv 走 npx，缓存目录固定，避免每次重下 ----------
$NpmCache = "C:\Users\$env:USERNAME\.npm-cache-lv"
$LvFontConvVersion = "1.5.2"

# ---------- 源字体 ----------
# simhei.ttf 是点阵黑体，14px 下比雅黑更清晰（已在小屏上实测过）
$FontSrc = "C:\Windows\Fonts\simhei.ttf"

Write-Host ""
Write-Host "===== 生成中文字库 =====" -ForegroundColor Cyan

if (-not (Test-Path $FontSrc)) {
    throw "找不到源字体: $FontSrc"
}
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# ==================================================================
# 1. 扫出所有汉字
#
# ⚠️ 扫描范围必须覆盖**所有可能出现中文的文件**，不能只扫 watch_strings.h：
#    曾经漏扫 watch_page_home.c 里的 "%d年%d月%d日"，结果"年、月"没进字库，
#    屏幕上就是两个豆腐块。这类 bug 不报错、只难看，很难查。
# ==================================================================
Write-Host "[1/4] 扫描源码中的中文"

$ScanRoots = @(
    (Join-Path $CompDir "watch_ui_pages"),
    (Join-Path $CompDir "watch_ui"),
    (Join-Path $CompDir "app")
)
$ScanFiles = @()
foreach ($root in $ScanRoots) {
    if (Test-Path $root) {
        $ScanFiles += (Get-ChildItem $root -Recurse -Include "*.c", "*.h" |
                       ForEach-Object { $_.FullName })
    }
}
$ScanFiles = $ScanFiles | Select-Object -Unique
if ($ScanFiles.Count -eq 0) {
    throw "没找到要扫描的源码文件，检查路径: $($ScanRoots -join ', ')"
}

# 匹配 CJK 统一汉字区（0x4E00-0x9FFF）+ 中文标点（0x3000-0x303F）+ 全角（0xFF00-0xFFEF）
$chars = [System.Collections.Generic.HashSet[string]]::new()
foreach ($file in $ScanFiles) {
    $content = Get-Content -Path $file -Raw -Encoding UTF8

    # ⚠️ 只扫**字符串字面量**里的中文，不扫注释。两个原因：
    #   1) 注释里的中文永远不会渲染，抽进去纯浪费 Flash
    #   2) 字符数会爆炸 → npx 报 "The command line is too long"
    #      （Windows 命令行上限 8191 字符，每个字要占约 8 个字符的 -r 0xXXXX）
    #      实测扫全文件能收到 839 个字（注释贡献 600+），改成只扫字面量后降到 200 左右。
    foreach ($lit in [regex]::Matches($content, '"([^"\\\r\n]|\\.)*"')) {
        foreach ($m in [regex]::Matches($lit.Value, '[\u4E00-\u9FFF\u3000-\u303F\uFF00-\uFFEF]')) {
            [void]$chars.Add($m.Value)
        }
    }
}

# 显式补充少量"非 CJK 区、但界面真的会显示"的字符。
# 扫描器的正则是 CJK 区（4E00-9FFF / 3000-303F / FF00-FFEF），
# 而省略号 U+2026 在"常用标点"区，扫不到 —— 但 WiFi 页的
# "扫描中…""连接中…" 都会用它，漏了就是豆腐块。
# 这类字符很少，直接列出来比放宽正则更可控（放宽会把 Emoji 等一大票都吸进来）。
$ExtraChars = @([char]0x2026)   # …
foreach ($c in $ExtraChars) { [void]$chars.Add([string]$c) }

if ($chars.Count -eq 0) {
    throw "一个汉字都没扫到 —— 上面的扫描路径对吗？"
}

# 排序让输出稳定（否则每次生成的 .c 都不一样，git 里全是假 diff）
$charList = @($chars) | Sort-Object
Write-Host "      扫描 $($ScanFiles.Count) 个文件，得到 $($charList.Count) 个不重复汉字/标点" -ForegroundColor Green

# 拼成 -r 0xXXXX 形式
$rangeArgs = @()
foreach ($c in $charList) {
    $code = [int][char]$c
    $rangeArgs += "-r"
    $rangeArgs += ("0x{0:X}" -f $code)
}

# 给个提示：把字列出来，加字时一眼能看出漏没漏
Write-Host "      字符: $($charList -join '')" -ForegroundColor DarkGray

# ==================================================================
# 2. 确保 lv_font_conv 可用
# ==================================================================
Write-Host "[2/4] 准备 lv_font_conv@$LvFontConvVersion（首次会联网下载）"

if (-not (Test-Path $NpmCache)) {
    New-Item -ItemType Directory -Path $NpmCache -Force | Out-Null
}

# ==================================================================
# 3. 逐档生成
# ==================================================================
Write-Host "[3/4] 生成字体"

foreach ($px in $Size) {
    $outFile = Join-Path $OutDir "lv_font_watch_$px.c"

    if ((Test-Path $outFile) -and (-not $Force)) {
        Write-Host "      ${px}px 已存在，跳过（要重生成加 -Force）" -ForegroundColor DarkGray
        continue
    }

    Write-Host "      ${px}px → $outFile" -ForegroundColor Yellow

    # --font 是**源字体路径**（lv_font_conv 1.5.2 没有"字体名"参数！）
    #        生成出来的字体对象名取自输出文件名：
    #        lv_font_watch_14.c → lv_font_watch_14
    # --bpp 4：4 位抗锯齿，小屏上中文笔画才不会糊成一团
    # --no-compress：LVGL 8.x 必须，见文件头说明
    #
    # ⚠️ 变量别叫 $args —— 那是 PowerShell 的自动变量（当前脚本的参数数组），
    #    赋值会被忽略，导致 npx 收到的是空参数、静默什么都不干。
    $convArgs = @(
        "--yes", "--cache", $NpmCache, "lv_font_conv@$LvFontConvVersion",
        "--font", $FontSrc,
        "--size", $px,
        "--bpp", "4",
        "--no-compress",
        "--format", "lvgl",
        "-o", $outFile,
        "--range", "0x20-0x7F"      # ASCII 也要，中文和数字常在同一行
    ) + $rangeArgs

    & npx @convArgs
    if ($LASTEXITCODE -ne 0) {
        throw "lv_font_conv 生成 ${px}px 失败（退出码 $LASTEXITCODE）"
    }
}

# ==================================================================
# 4. 校验产物
# ==================================================================
Write-Host "[4/4] 校验产物"

$ok = $true
foreach ($px in $Size) {
    $outFile = Join-Path $OutDir "lv_font_watch_$px.c"
    if (-not (Test-Path $outFile)) {
        Write-Host "      ✗ 缺少 lv_font_watch_$px.c" -ForegroundColor Red
        $ok = $false
        continue
    }

    $sizeKB = [math]::Round((Get-Item $outFile).Length / 1KB, 1)

    # 抽查：必须包含字体对象定义
    $hasObj = Select-String -Path $outFile -Pattern "const lv_font_t lv_font_watch_$px" -Quiet
    if (-not $hasObj) {
        Write-Host "      ✗ ${px}px 文件里没有 lv_font_watch_$px 定义（--font 名字不对？）" -ForegroundColor Red
        $ok = $false
        continue
    }

    Write-Host "      ✓ lv_font_watch_$px.c  ($sizeKB KB)" -ForegroundColor Green
}

if (-not $ok) {
    throw "字库生成不完整，见上面的错误"
}

Write-Host ""
Write-Host "完成。现在可以编译烧录了：" -ForegroundColor Cyan
Write-Host "    cd $ProjectRoot" -ForegroundColor White
Write-Host "    .\build.ps1 monitor" -ForegroundColor White
Write-Host ""
Write-Host "提示：以后往界面加新中文，只要写进 watch_strings.h 再跑一次本脚本。" -ForegroundColor DarkGray
Write-Host ""
