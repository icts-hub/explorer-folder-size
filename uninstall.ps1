# 卸载：移除右键菜单注册
$ErrorActionPreference = 'SilentlyContinue'

$keys = @(
    'HKCU:\Software\Classes\Directory\shell\FolderSizeView',
    'HKCU:\Software\Classes\Directory\Background\shell\FolderSizeView',
    'HKCU:\Software\Classes\Drive\shell\FolderSizeView',
    'HKCU:\Software\Classes\Folder\shellex\ColumnHandlers\{8F3E2A71-4C9B-4E2D-9A7B-3C1D5E8F0A42}',
    'HKCU:\Software\Classes\CLSID\{8F3E2A71-4C9B-4E2D-9A7B-3C1D5E8F0A42}'
)

Write-Host ""
Write-Host "  FolderSize 卸载程序" -ForegroundColor Cyan
Write-Host ""

$removed = 0
foreach ($k in $keys) {
    if (Test-Path $k) {
        Remove-Item $k -Recurse -Force
        Write-Host "  [OK] 已移除 $k" -ForegroundColor Green
        $removed++
    }
}

if ($removed -eq 0) {
    Write-Host "  没有找到注册项，可能已经卸载过了。" -ForegroundColor DarkGray
}

# 清理残留临时文件
$leftovers = Get-ChildItem ([System.IO.Path]::GetTempPath()) -Filter 'foldersize_*.tsv' -ErrorAction SilentlyContinue
if ($leftovers) {
    $leftovers | Remove-Item -Force -ErrorAction SilentlyContinue
    Write-Host "  [OK] 已清理 $($leftovers.Count) 个临时文件" -ForegroundColor Green
}

Write-Host ""
Write-Host "  卸载完成（右键菜单立即消失）。" -ForegroundColor Cyan
Write-Host ""
Read-Host "  按回车退出"
