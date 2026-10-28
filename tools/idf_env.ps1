# ESP-IDF 环境准备（本机专用）—— dot-source 使用：
#
#   . .\tools\idf_env.ps1
#   idf.py build          # 或 idf.py -p COM4 flash / monitor
#
# ⚠️ 本文件必须保存为 **UTF-8 with BOM**。
#    Windows PowerShell 5.1 读取无 BOM 的 .ps1 时按 ANSI(GBK) 解码，
#    中文注释会变成乱码并可能吃掉引号，导致整个脚本解析失败。
#
# 为什么需要它
# ------------
# 本机的 ESP-IDF 安装**不在默认布局上**，直接用官方 export.ps1 会连撞两个坑：
#
#   1. Python venv 位于 IDF 6.x 的**新布局**
#        $IDF_TOOLS_PATH\tools\python\v<ver>\venv
#      而 export.ps1 / install.ps1 期望旧布局
#        $IDF_TOOLS_PATH\python_env\idf<ver>_py<ver>_env
#      -> 报 "ESP-IDF Python virtual environment ... not found"。
#      idf_tools.py 的 get_python_env_path() 允许用 IDF_PYTHON_ENV_PATH 覆盖。
#
#   2. idf.py 启动时校验 Python 依赖的 constraints 文件
#        $IDF_TOOLS_PATH\espidf.constraints.v<ver>.txt
#      本机该文件实际在 $IDF_TOOLS_PATH\tools\ 下 -> 报
#      "espidf.constraints.v6.0.txt doesn't exist. Perhaps you've forgotten
#       to run the install scripts."
#      用 IDF_PYTHON_CHECK_CONSTRAINTS=0 关闭该检查。
#
# 本脚本只做上述两件事 + 用 idf_tools.py export 取回工具链 PATH，
# 不改动 ESP-IDF 安装本身、不下载任何组件。

[CmdletBinding()]
param(
    [string]$IdfPath   = $env:IDF_PATH,
    [string]$ToolsPath = $env:IDF_TOOLS_PATH
)

if (-not $IdfPath)   { $IdfPath   = 'F:\esp\v6.0.2\esp-idf' }
if (-not $ToolsPath) { $ToolsPath = 'F:\Espressif' }

$idfPy = Join-Path $IdfPath 'tools\idf.py'
if (-not (Test-Path $idfPy)) {
    Write-Error "找不到 $idfPy —— 请用 -IdfPath 指定 ESP-IDF 根目录"
    return
}

# 在新布局里找 venv：$ToolsPath\tools\python\v<ver>\venv
$venv = Get-ChildItem (Join-Path $ToolsPath 'tools\python') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'venv' } |
        Where-Object { Test-Path (Join-Path $_ 'Scripts\python.exe') } |
        Select-Object -First 1
if (-not $venv) {
    Write-Error "在 $ToolsPath\tools\python\*\venv 下找不到 Python venv —— 请用 -ToolsPath 指定"
    return
}
$py = Join-Path $venv 'Scripts\python.exe'

$env:IDF_PATH                     = $IdfPath
$env:IDF_TOOLS_PATH               = $ToolsPath
$env:IDF_PYTHON_ENV_PATH          = $venv    # 覆盖坑 1
$env:IDF_PYTHON_CHECK_CONSTRAINTS = '0'      # 覆盖坑 2

# 取回工具链（xtensa-esp-elf / cmake / ninja / esptool ...）的 PATH 与其它变量
$exports = & $py (Join-Path $IdfPath 'tools\idf_tools.py') export --format key-value 2>$null
foreach ($line in $exports) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $k = $Matches[1]; $v = $Matches[2]
        if ($k -eq 'PATH') { $env:PATH = "$v;$env:PATH" } else { Set-Item -Path "env:$k" -Value $v }
    }
}
# venv 的 Scripts 排在最前：idf.py 的子步骤会直接调用 python
$env:PATH = "$(Join-Path $venv 'Scripts');$env:PATH"

Write-Host "ESP-IDF 环境就绪" -ForegroundColor Green
Write-Host "  IDF_PATH                     = $env:IDF_PATH"
Write-Host "  IDF_TOOLS_PATH               = $env:IDF_TOOLS_PATH"
Write-Host "  IDF_PYTHON_ENV_PATH          = $env:IDF_PYTHON_ENV_PATH"
Write-Host "  IDF_PYTHON_CHECK_CONSTRAINTS = $env:IDF_PYTHON_CHECK_CONSTRAINTS"
Write-Host "  现在可以直接用: idf.py build / idf.py -p COM4 flash monitor"