# FolderSize

在 Windows 资源管理器里一键查看文件夹大小 —— 右键文件夹，弹出按大小降序排列的明细窗口。

Windows 自带的「大小」列对文件夹永远是空的。这个工具补上这个能力，用来找是什么吃掉了磁盘空间。

```
┌─ 文件夹大小 - C:\Users\me\Documents ─────────────────────────┐
│ C:\Users\me\Documents                                        │
│ ┌──────────────────────────────┬──────────┬────────┐         │
│ │ 文件夹                        │     大小 │  占比  │         │
│ ├──────────────────────────────┼──────────┼────────┤         │
│ │ node_modules                 │  5.74 GB │ 45.6%  │         │
│ │ Video Projects               │  3.66 GB │ 29.1%  │         │
│ │ Game Assets                  │  1.64 GB │ 13.0%  │         │
│ │ Music Library                │ 608.6 MB │  4.7%  │         ││ │ ...                          │          │        │         │
│ └──────────────────────────────┴──────────┴────────┘         │
│ 共 40 项，总计 12.58 GB   |   C:\ 剩余 25.40 GB              │
│                              [打开选中] [复制结果] [重新统计] │
└──────────────────────────────────────────────────────────────┘
```

## 快速开始

```bat
build.bat      :: 编译（需要 MinGW-w64）
install.bat    :: 注册右键菜单（写 HKCU，不需要管理员权限）
```

装完立即可用，**不需要重启资源管理器**。

### 三种入口

| 操作 | 效果 |
| --- | --- |
| 右键**文件夹** → 查看文件夹大小 | 列出该文件夹下所有子文件夹的大小 |
| 在文件夹**空白处**右键 → 统计当前文件夹大小 | 统计当前正在浏览的目录 |
| 右键 **C: / D:** 驱动器 → 查看文件夹大小 | 统计整个磁盘 |

窗口内可以双击进入子文件夹、复制结果、重新统计。

卸载：`uninstall.bat`，右键菜单立即消失。

## 为什么不做成资源管理器的列

最初实现的是真正的 COM 列扩展（[`IColumnProvider`](https://learn.microsoft.com/windows/win32/api/shlobj/nn-shlobj-icolumnprovider)），
代码保留在 `src/FolderSizeExt.cpp`。

**但在 Windows 11 build 26200 上不生效。** 排查结论：

| 检查项 | 结果 |
| --- | --- |
| 注册位置 | 正确 —— `HKCU\Software\Classes\Folder\shellex\ColumnHandlers\{CLSID}`，与已安装的第三方列扩展同处 |
| DLL 依赖 | 干净 —— 导入表只有 `KERNEL32` / `OLEAUT32` / `SHELL32` / `api-ms-win-crt-*`，无 MinGW 运行时 |
| COM 激活 | `CoCreateInstance` 返回 `S_OK`，13 项接口断言全部通过 |
| Explorer 加载 | **从未加载** —— `explorer.exe` 的模块列表里没有该 DLL，列标题菜单里也没有该项 |

即系统根本没有去请求这个列。`IColumnProvider` 从 Vista 起即为已废弃接口，新版资源管理器
不再枚举 `ColumnHandlers`。所以最终改为右键菜单方案。

`install.ps1` 会主动清理掉这个失效的注册项。列扩展代码保留在仓库中作为参考。

## 实现要点

### 扫描器：多线程 + 迭代而非递归

```
bin\FolderSizeCLI.exe <目录> [输出.tsv]
```

按一级子目录拆任务，线程池并行扫描（线程数 = CPU 核数，上限 8）。
每个子目录内部用显式栈迭代，避免深层目录把调用栈打爆。

实测性能：

| 目标 | 规模 | 耗时 |
| --- | --- | --- |
| 用户主目录 | 48.8 GB | 约 11 秒 |
| 整个系统盘 | 167 GB，33 个一级目录 | 约 20 秒 |

### 结果写文件，而不是走 stdout 管道

GUI 用 `ProcessStartInfo` 启动扫描器。如果靠 stdout 管道回传结果，子文件夹一多
（输出超过管道缓冲区约 4 KB），子进程就阻塞在写操作上，父进程又在等它退出 —— 死锁。

所以扫描器把结果直接写进临时 TSV 文件，GUI 轮询进程退出后读文件。

### Windows 命令行：路径结尾反斜杠的坑

注册表菜单命令形如：

```
powershell.exe ... -File "FolderSizeGui.ps1" "%1"
```

对驱动器（`C:\`）而言，替换后变成 `"...\FolderSizeGui.ps1" "C:\"`。
Windows 命令行规则里 `\"` 是**转义的引号**，参数会被解析成 `C:"`，路径就坏了 ——
表现是窗口能打开但列表一片空白。

两处处理：

- **GUI 入口**：`TrimEnd('"')` 还原本质路径
- **传给扫描器**：结尾是反斜杠时补 `\.`（`C:\` → `C:\.`），语义相同但不会被吞

扫描器再用 `GetFullPathNameW` 规范化，所以展示的路径依然是干净的 `C:\xxx`。

### 编码约定

面向中文 Windows（代码页 936 / GBK）时容易踩坑，本项目约定：

- 所有 `.bat` 保持**纯 ASCII** —— 批处理里出现中文会乱码，还会把命令拆碎执行
- 中文文本一律放 `.ps1`，且存为 **UTF-8 with BOM**，否则 PowerShell 5.1 读不对
- 扫描器输出统一 UTF-8；PowerShell 侧用 `[System.IO.File]::ReadAllText(..., UTF8)` 读取

## 目录结构

```
.
├── src/
│   ├── FolderSizeCLI.cpp    多线程扫描器（核心）
│   ├── FolderSizeExt.cpp    IColumnProvider 列扩展（Win11 不生效，保留参考）
│   └── test.cpp             COM 自测程序
├── FolderSizeGui.ps1        WinForms 明细窗口
├── build.bat                编译
├── install.bat / .ps1       安装
├── uninstall.bat / .ps1     卸载
└── bin/                     编译产物（未纳入版本控制）
```

## 验证

扫描结果与 PowerShell `Get-ChildItem -Recurse | Measure-Object Length -Sum` 对照：

| 目标 | FolderSizeCLI | PowerShell |
| --- | --- | --- |
| 源码目录 | 20.9 KB | 20.9 KB |
| `C:\ProgramData` | 12.60 GB | 12.60 GB |
| 用户主目录 | 48.82 GB | 48.83 GB |
| 文档目录 | 12.57 GB | 12.57 GB |

计算口径：递归累加所有文件大小，不含目录元数据；跳过符号链接与联接点（不跟随）；
受当前用户权限限制，无权限的子目录会被跳过，因此可能略小于其他工具的结果。

## 排查

**右键没有「查看文件夹大小」**
检查 `HKCU\Software\Classes\Directory\shell\FolderSizeView` 是否存在，没有就重跑 `install.bat`。

**窗口打开但列表空白**
状态栏会显示 `扫描失败 (退出码 N)`。手动跑一次看具体报错：

```bat
bin\FolderSizeCLI.exe "C:\some\folder" %TEMP%\o.tsv
```

**数值比其他工具小**
正常 —— 无权限的子目录会被跳过。用管理员身份运行可缩小差异。

## 环境

- Windows 10 / 11 x64
- 编译需要 MinGW-w64（`g++` 支持 C++17，x86_64 目标）
- 运行只需要系统自带的 PowerShell 5.1 与 .NET Framework

## License

[MIT](LICENSE)
