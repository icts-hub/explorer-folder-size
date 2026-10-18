# 安装：注册文件夹右键菜单
#   - 右键文件夹  -> 「查看文件夹大小」
#   - 右键空白处  -> 「统计当前文件夹大小」
#   - 右键驱动器  -> 「查看文件夹大小」
# 全部写入 HKCU，不需要管理员权限

$ErrorActionPreference = 'Stop'

$gui      = Join-Path $PSScriptRoot 'FolderSizeGui.ps1'
$cli      = Join-Path $PSScriptRoot 'bin\FolderSizeCLI.exe'
$oldClsid = '{8F3E2A71-4C9B-4E2D-9A7B-3C1D5E8F0A42}'

Write-Host ""
Write-Host "  FolderSize —— 文件夹大小 安装程序" -ForegroundColor Cyan
Write-Host ("  " + ("=" * 52)) -ForegroundColor DarkCyan
Write-Host ""

# ---------- 前置检查 ----------
$ok = $true
foreach ($f in @($gui, $cli)) {
    if (Test-Path $f) {
        Write-Host "  [OK] $([System.IO.Path]::GetFileName($f))" -ForegroundColor Green
    } else {
        Write-Host "  [缺失] $f" -ForegroundColor Red
        $ok = $false
    }
}
if (-not $ok) {
    Write-Host ""
    Write-Host "  请先运行 build.bat 编译。" -ForegroundColor Yellow
    Read-Host "  按回车退出"
    exit 1
}

# ---------- 清理旧的 IColumnProvider 注册 ----------
# Windows 11 build 26200 不再支持列扩展，注册了也不会显示，一并清掉
foreach ($k in @(
    "HKCU:\Software\Classes\Folder\shellex\ColumnHandlers\$oldClsid",
    "HKCU:\Software\Classes\CLSID\$oldClsid"
)) {
    if (Test-Path $k) {
        Remove-Item $k -Recurse -Force
        Write-Host "  [清理] 移除了失效的列扩展注册" -ForegroundColor DarkGray
    }
}

# ---------- 生成命令 ----------
$psExe = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'

function Make-Cmd([string]$placeholder) {
    '"{0}" -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "{1}" "{2}"' -f `
        $psExe, $gui, $placeholder
}

$entries = @(
    @{ Key = 'Directory\shell\FolderSizeView';            Text = '查看文件夹大小';     Ph = '%1' },
    @{ Key = 'Directory\Background\shell\FolderSizeView'; Text = '统计当前文件夹大小'; Ph = '%V' },
    @{ Key = 'Drive\shell\FolderSizeView';                Text = '查看文件夹大小';     Ph = '%1' }
)

foreach ($e in $entries) {
    $base = "HKCU:\Software\Classes\$($e.Key)"
    New-Item -Path $base -Force | Out-Null
    Set-ItemProperty -Path $base -Name '(default)' -Value $e.Text

    $cmdKey = "$base\command"
    New-Item -Path $cmdKey -Force | Out-Null
    Set-ItemProperty -Path $cmdKey -Name '(default)' -Value (Make-Cmd $e.Ph)
    Write-Host "  [OK] 已注册: $($e.Text)" -ForegroundColor Green
}

# ---------- 回读校验 ----------
$check = (Get-ItemProperty -Path 'HKCU:\Software\Classes\Directory\shell\FolderSizeView\command' -Name '(default)').'(default)'
if ($check -notlike '*FolderSizeGui.ps1*') {
    Write-Host "  [错误] 回读校验失败: $check" -ForegroundColor Red
    Read-Host "  按回车退出"
    exit 1
}
Write-Host "  [OK] 注册回读校验通过" -ForegroundColor Green

Write-Host ""
Write-Host ("  " + ("=" * 52)) -ForegroundColor DarkCyan
Write-Host "  安装完成（不需要重启资源管理器）" -ForegroundColor Cyan
Write-Host ("  " + ("=" * 52)) -ForegroundColor DarkCyan
Write-Host ""
Write-Host "  用法：" -ForegroundColor White
Write-Host "    · 右键任意文件夹   -> 查看文件夹大小" -ForegroundColor Gray
Write-Host "    · 在文件夹空白处右键 -> 统计当前文件夹大小" -ForegroundColor Gray
Write-Host "    · 右键 C: / D: 驱动器 -> 查看文件夹大小" -ForegroundColor Gray
Write-Host ""
Write-Host "  窗口内可双击进入子文件夹、复制结果、重新统计。" -ForegroundColor Gray
Write-Host ""
Read-Host "  按回车退出"
