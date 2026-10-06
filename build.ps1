# ============================================================
#  智能手表 — 一键编译脚本
#  用法:
#     .\build.ps1              # 编译
#     .\build.ps1 flash        # 编译并烧录
#     .\build.ps1 monitor      # 编译+烧录+看日志
#     .\build.ps1 clean        # 彻底清理
#  端口用 -Port 指定，例如 .\build.ps1 flash -Port COM5
# ============================================================
param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "flash", "monitor", "clean", "menuconfig")]
    [string]$Action = "build",
    [string]$Port = ""
)

$ErrorActionPreference = "Continue"

# ---- 关键环境变量（缺一不可）----
$env:IDF_COMPONENT_MANAGER = "0"      # 关闭组件管理器（组件已本地化到 components/）
$env:LVGL_VERSION          = "8.4.0"  # esp_lvgl_port 靠它判断用 lvgl8 还是 lvgl9 源码
$env:PYTHONUTF8            = "1"      # 避免中文路径/输出编码问题

# ---- 激活 ESP-IDF ----
$idfInit = "D:\my\DevelopTool\Espressif\Initialize-Idf.ps1"
$idfId   = "esp-idf-a93edf6725679030a0fa36cf7f6a9eb1"

if (-not (Test-Path $idfInit)) {
    Write-Host "找不到 ESP-IDF 初始化脚本: $idfInit" -ForegroundColor Red
    exit 1
}

. $idfInit -IdfId $idfId *>&1 | Out-Null
$env:IDF_COMPONENT_MANAGER = "0"
$env:LVGL_VERSION          = "8.4.0"

Set-Location $PSScriptRoot

switch ($Action) {
    "clean" {
        Remove-Item "build" -Recurse -Force -ErrorAction SilentlyContinue
        Remove-Item "sdkconfig" -Force -ErrorAction SilentlyContinue
        Write-Host "已清理 build/ 和 sdkconfig" -ForegroundColor Green
    }
    "menuconfig" {
        idf.py menuconfig
    }
    "build" {
        idf.py build
    }
    "flash" {
        if ($Port) { idf.py -p $Port flash } else { idf.py flash }
    }
    "monitor" {
        if ($Port) { idf.py -p $Port flash monitor } else { idf.py flash monitor }
    }
}
