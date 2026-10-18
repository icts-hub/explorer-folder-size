# FolderSizeGui.ps1 —— 文件夹大小明细窗口
# 由右键菜单调用，参数为目标文件夹路径
#
# 数据交换方式：CLI 把结果写入临时文件，本窗口轮询进程退出后读取。
# 不走 stdout 管道，避免子文件夹很多时撑爆管道缓冲区导致死锁。

param([string]$TargetPath)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# ---------- 参数归一化 ----------
# 从注册表菜单传入 "C:\" 时，命令行会把结尾引号吃掉，变成 'C:"'
if ([string]::IsNullOrWhiteSpace($TargetPath)) { exit 1 }
$TargetPath = $TargetPath.TrimEnd('"')
if ($TargetPath -match '^[A-Za-z]:$') { $TargetPath += '\' }
$TargetPath = $TargetPath.TrimEnd('\')
if ($TargetPath -match '^[A-Za-z]:$') { $TargetPath += '\' }

if (-not (Test-Path -LiteralPath $TargetPath)) {
    [System.Windows.Forms.MessageBox]::Show("路径不存在：`n$TargetPath", "文件夹大小",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    exit 1
}

$cli = Join-Path $PSScriptRoot 'bin\FolderSizeCLI.exe'
if (-not (Test-Path $cli)) {
    [System.Windows.Forms.MessageBox]::Show("找不到扫描程序：`n$cli`n`n请先运行 build.bat",
        "文件夹大小",
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error) | Out-Null
    exit 1
}

# ---------- 窗体 ----------
$form               = New-Object System.Windows.Forms.Form
$form.Text          = "文件夹大小 - $TargetPath"
$form.Size          = New-Object System.Drawing.Size(940, 640)
$form.MinimumSize   = New-Object System.Drawing.Size(660, 440)
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$form.Font          = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)
$form.ShowIcon      = $false

# ---------- 布局: TableLayoutPanel 保证尺寸确定 ----------
$tlp             = New-Object System.Windows.Forms.TableLayoutPanel
$tlp.Dock        = [System.Windows.Forms.DockStyle]::Fill
$tlp.ColumnCount = 1
$tlp.RowCount    = 4
$tlp.Padding     = New-Object System.Windows.Forms.Padding(0)

[void]$tlp.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle(
    [System.Windows.Forms.SizeType]::Percent, 100)))
[void]$tlp.RowStyles.Add((New-Object System.Windows.Forms.RowStyle(
    [System.Windows.Forms.SizeType]::Absolute, 34)))
[void]$tlp.RowStyles.Add((New-Object System.Windows.Forms.RowStyle(
    [System.Windows.Forms.SizeType]::Absolute, 6)))
[void]$tlp.RowStyles.Add((New-Object System.Windows.Forms.RowStyle(
    [System.Windows.Forms.SizeType]::Percent, 100)))
[void]$tlp.RowStyles.Add((New-Object System.Windows.Forms.RowStyle(
    [System.Windows.Forms.SizeType]::Absolute, 54)))

# --- 路径栏 ---
$lblPath              = New-Object System.Windows.Forms.Label
$lblPath.Dock         = [System.Windows.Forms.DockStyle]::Fill
$lblPath.Text         = $TargetPath
$lblPath.TextAlign    = [System.Drawing.ContentAlignment]::MiddleLeft
$lblPath.Padding      = New-Object System.Windows.Forms.Padding(10, 0, 10, 0)
$lblPath.Font         = New-Object System.Drawing.Font('Microsoft YaHei UI', 9,
                          [System.Drawing.FontStyle]::Bold)
$lblPath.AutoEllipsis = $true

# --- 进度条 ---
$progress        = New-Object System.Windows.Forms.ProgressBar
$progress.Dock   = [System.Windows.Forms.DockStyle]::Fill
$progress.Style  = [System.Windows.Forms.ProgressBarStyle]::Marquee
$progress.MarqueeAnimationSpeed = 30

# --- 列表 ---
$list               = New-Object System.Windows.Forms.ListView
$list.Dock          = [System.Windows.Forms.DockStyle]::Fill
$list.View          = [System.Windows.Forms.View]::Details
$list.FullRowSelect = $true
$list.GridLines     = $true
$list.HideSelection = $false
$list.Sorting       = [System.Windows.Forms.SortOrder]::None
[void]$list.Columns.Add('文件夹', 440, [System.Windows.Forms.HorizontalAlignment]::Left)
[void]$list.Columns.Add('大小',   120, [System.Windows.Forms.HorizontalAlignment]::Right)
[void]$list.Columns.Add('占比',    90, [System.Windows.Forms.HorizontalAlignment]::Right)

# --- 底部状态栏 ---
$status        = New-Object System.Windows.Forms.Panel
$status.Dock   = [System.Windows.Forms.DockStyle]::Fill
$status.Padding = New-Object System.Windows.Forms.Padding(8, 8, 8, 8)

$lblStatus        = New-Object System.Windows.Forms.Label
$lblStatus.Dock   = [System.Windows.Forms.DockStyle]::Fill
$lblStatus.Text   = '正在统计…'
$lblStatus.TextAlign = [System.Drawing.ContentAlignment]::MiddleLeft

$btnPanel          = New-Object System.Windows.Forms.FlowLayoutPanel
$btnPanel.Dock     = [System.Windows.Forms.DockStyle]::Right
$btnPanel.Width    = 312
$btnPanel.FlowDirection = [System.Windows.Forms.FlowDirection]::RightToLeft

$btnOpen         = New-Object System.Windows.Forms.Button
$btnOpen.Text    = '打开选中'
$btnOpen.Size    = New-Object System.Drawing.Size(92, 30)
$btnOpen.Enabled = $false

$btnCopy         = New-Object System.Windows.Forms.Button
$btnCopy.Text    = '复制结果'
$btnCopy.Size    = New-Object System.Drawing.Size(92, 30)
$btnCopy.Enabled = $false

$btnAgain         = New-Object System.Windows.Forms.Button
$btnAgain.Text    = '重新统计'
$btnAgain.Size    = New-Object System.Drawing.Size(92, 30)
$btnAgain.Enabled = $false

[void]$btnPanel.Controls.Add($btnAgain)
[void]$btnPanel.Controls.Add($btnCopy)
[void]$btnPanel.Controls.Add($btnOpen)
[void]$status.Controls.Add($lblStatus)
[void]$status.Controls.Add($btnPanel)

[void]$tlp.Controls.Add($lblPath,  0, 0)
[void]$tlp.Controls.Add($progress, 0, 1)
[void]$tlp.Controls.Add($list,     0, 2)
[void]$tlp.Controls.Add($status,   0, 3)
[void]$form.Controls.Add($tlp)

# ---------- 状态 ----------
$script:proc  = $null
$script:tmp   = $null
$script:rows  = @()
$script:total = 0
$script:lines = New-Object System.Collections.ArrayList

function Format-Size([double]$n) {
    if ($n -lt 1024) { return ('{0:N0} B'  -f $n) }
    if ($n -lt 1MB)  { return ('{0:N1} KB' -f ($n / 1KB)) }
    if ($n -lt 1GB)  { return ('{0:N1} MB' -f ($n / 1MB)) }
    if ($n -lt 1TB)  { return ('{0:N2} GB' -f ($n / 1GB)) }
    return ('{0:N2} TB' -f ($n / 1TB))
}

function Clear-OldTemp {
    if ($script:tmp -and (Test-Path -LiteralPath $script:tmp)) {
        Remove-Item -LiteralPath $script:tmp -Force -ErrorAction SilentlyContinue
    }
    $script:tmp = $null
}

function Start-Scan {
    $timer.Stop()
    try { if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() } } catch {}
    Clear-OldTemp

    $list.Items.Clear()
    $script:lines.Clear()
    $script:rows       = @()
    $script:total      = 0
    $lblStatus.Text    = '正在统计…'
    $btnOpen.Enabled   = $false
    $btnCopy.Enabled   = $false
    $btnAgain.Enabled  = $false
    $progress.Style    = [System.Windows.Forms.ProgressBarStyle]::Marquee
    $form.Cursor       = [System.Windows.Forms.Cursors]::WaitCursor

    $script:tmp = Join-Path ([System.IO.Path]::GetTempPath()) `
                            ("foldersize_{0}.tsv" -f [guid]::NewGuid().ToString('N'))

    # 命令行参数里 "C:\" 结尾的 \" 会被当作转义引号吃掉,
    # 所以结尾有反斜杠时补一个 "." -> "C:\." 语义相同但不会被吃掉
    $cliArg = $TargetPath
    if ($cliArg.EndsWith('\')) { $cliArg = $cliArg.TrimEnd('\') + '\.' }

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName        = $cli
    $psi.Arguments       = '"' + $cliArg + '" "' + $script:tmp + '"'
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow  = $true

    try {
        $script:proc = [System.Diagnostics.Process]::Start($psi)
        $timer.Start()
    } catch {
        $lblStatus.Text   = "启动失败: $($_.Exception.Message)"
        $progress.Style   = [System.Windows.Forms.ProgressBarStyle]::Blocks
        $form.Cursor      = [System.Windows.Forms.Cursors]::Default
    }
}

function Complete-Scan {
    $timer.Stop()
    $form.Cursor = [System.Windows.Forms.Cursors]::Default

    $out = ''
    if ($script:tmp -and (Test-Path -LiteralPath $script:tmp)) {
        try {
            $out = [System.IO.File]::ReadAllText($script:tmp, [System.Text.Encoding]::UTF8)
        } catch {
            $lblStatus.Text = "读取结果失败: $($_.Exception.Message)"
            return
        }
    }

    # 扫描器异常退出 (exit code != 0) 且没有输出 -> 明确报错, 不显示 "0 项"
    $exitCode = -1
    try { $exitCode = $script:proc.ExitCode } catch {}
    if ([string]::IsNullOrWhiteSpace($out) -and $exitCode -ne 0) {
        $lblStatus.Text   = "扫描失败 (退出码 $exitCode)，路径可能不存在或无权限: $TargetPath"
        $progress.Style   = [System.Windows.Forms.ProgressBarStyle]::Blocks
        $progress.Value   = 100
        $btnAgain.Enabled = $true
        return
    }

    $rows  = New-Object System.Collections.ArrayList
    $total = 0
    foreach ($line in ($out -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 3) { continue }
        if ($parts[0] -eq 'TOTAL') { $total = [double]$parts[1]; continue }
        [void]$rows.Add([PSCustomObject]@{
            Name  = $parts[1]
            Path  = $parts[2]
            Bytes = [double]$parts[0]
        })
    }

    if ($total -le 0) {
        $sum = ($rows | Measure-Object -Property Bytes -Sum).Sum
        if ($sum) { $total = [double]$sum }
    }
    if (-not $total) { $total = 0 }

    $script:total = $total
    $script:rows  = $rows

    $list.BeginUpdate()
    $list.Items.Clear()
    foreach ($r in ($rows | Sort-Object -Property Bytes -Descending)) {
        $pct = if ($total -gt 0) { '{0:N1}%' -f ($r.Bytes / $total * 100) } else { '-' }
        $it = New-Object System.Windows.Forms.ListViewItem($r.Name)
        [void]$it.SubItems.Add((Format-Size $r.Bytes))
        [void]$it.SubItems.Add($pct)
        $it.Tag = $r.Path
        [void]$list.Items.Add($it)
        [void]$script:lines.Add(('{0}`t{1}' -f (Format-Size $r.Bytes), $r.Name))
    }
    $list.EndUpdate()

    $driveInfo = ''
    try {
        $rootPath  = [System.IO.Path]::GetPathRoot($TargetPath)
        $di        = New-Object System.IO.DriveInfo($rootPath)
        $driveInfo = "   |   $($di.Name) 剩余 $(Format-Size $di.AvailableFreeSpace)"
    } catch {}

    $lblStatus.Text   = "共 $($rows.Count) 项，总计 $(Format-Size $total)$driveInfo"
    $progress.Style   = [System.Windows.Forms.ProgressBarStyle]::Blocks
    $progress.Value   = 100
    $btnCopy.Enabled  = $true
    $btnAgain.Enabled = $true
}

# ---------- 事件 ----------
$timer          = New-Object System.Windows.Forms.Timer
$timer.Interval = 250
$timer.add_Tick({
    if ($script:proc -and $script:proc.HasExited) { Complete-Scan }
})

$list.add_SelectedIndexChanged({
    $btnOpen.Enabled = ($list.SelectedItems.Count -gt 0)
})

$list.add_DoubleClick({
    if ($list.SelectedItems.Count -gt 0) {
        $p = $list.SelectedItems[0].Tag
        if ($p -and (Test-Path -LiteralPath $p)) {
            Start-Process explorer.exe -ArgumentList ('"' + $p + '"')
        }
    }
})

$btnOpen.add_Click({
    if ($list.SelectedItems.Count -gt 0) {
        $p = $list.SelectedItems[0].Tag
        if ($p -and (Test-Path -LiteralPath $p)) {
            Start-Process explorer.exe -ArgumentList ('"' + $p + '"')
        }
    }
})

$btnCopy.add_Click({
    [System.Windows.Forms.Clipboard]::SetText(($script:lines -join "`r`n"))
    $lblStatus.Text = "已复制 $($script:lines.Count) 行到剪贴板"
})

$btnAgain.add_Click({ Start-Scan })

$form.add_Shown({ Start-Scan })

$form.add_FormClosed({
    $timer.Stop()
    try { if ($script:proc -and -not $script:proc.HasExited) { $script:proc.Kill() } } catch {}
    Clear-OldTemp
})

[void]$form.ShowDialog()
