// FolderSizeCLI - 多线程文件夹大小统计
// 用法: FolderSizeCLI.exe <目录>
// 输出: UTF-8 TSV, 每行  <字节数>\t<名称>\t<完整路径>
//       最后一行 TOTAL\t<字节数>\t<目录>

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>

#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 0x2
#endif

// ---------- 递归求大小 (跳过重解析点) ----------
static unsigned long long DirSize(const std::wstring& root)
{
    unsigned long long total = 0;
    std::vector<std::wstring> stack;
    stack.push_back(root);

    // 复用缓冲区, 避免每个目录都分配
    std::wstring pattern;
    while (!stack.empty())
    {
        std::wstring dir = stack.back();
        stack.pop_back();

        pattern.assign(dir);
        if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;

        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                std::wstring child = dir;
                if (!child.empty() && child.back() != L'\\') child += L'\\';
                child += fd.cFileName;
                stack.push_back(child);
            }
            else
            {
                total += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }
    return total;
}

// ---------- 并行扫描上下文 ----------
struct Task
{
    std::wstring         name;
    std::wstring         fullPath;
    unsigned long long   size;
};

static volatile LONG g_nextIndex = 0;
static Task*         g_tasks = nullptr;
static LONG           g_taskCount = 0;

static DWORD WINAPI WorkerProc(LPVOID)
{
    for (;;)
    {
        LONG i = InterlockedIncrement(&g_nextIndex) - 1;
        if (i >= g_taskCount) break;
        g_tasks[i].size = DirSize(g_tasks[i].fullPath);
    }
    return 0;
}

// ---------- UTF-8 输出 ----------
static std::string g_out;

static void AppendUtf8(const wchar_t* s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return;
    size_t old = g_out.size();
    g_out.resize(old + (size_t)(n - 1));
    WideCharToMultiByte(CP_UTF8, 0, s, -1, &g_out[old], n, nullptr, nullptr);
}

static void AppendUtf8(const std::wstring& s) { AppendUtf8(s.c_str()); }

static void AppendLine(unsigned long long size, const std::wstring& name, const std::wstring& path)
{
    char num[32];
    sprintf(num, "%llu", size);
    g_out += num;
    g_out += '\t';
    AppendUtf8(name);
    g_out += '\t';
    AppendUtf8(path);
    g_out += '\n';
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fputs("usage: FolderSizeCLI.exe <directory> [output.tsv]\n", stderr);
        return 2;
    }

    std::wstring root = argv[1];

    // 规范化路径: 抹平 "C:\. " / 多余反斜杠 / 相对路径 等写法
    {
        wchar_t full[32768];
        DWORD n = GetFullPathNameW(root.c_str(), 32768, full, NULL);
        if (n > 0 && n < 32768) root.assign(full);
    }

    // 去掉结尾反斜杠 (保留 "C:\" 这种)
    while (root.size() > 3 && root.back() == L'\\')
        root.pop_back();

    DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        fwprintf(stderr, L"not a directory: %ls\n", root.c_str());
        return 2;
    }

    // ---- 枚举一级子目录 ----
    std::vector<Task> tasks;
    unsigned long long directFiles = 0;

    std::wstring pattern = root;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            std::wstring child = root;
            if (!child.empty() && child.back() != L'\\') child += L'\\';
            child += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                Task t;
                t.name     = fd.cFileName;
                t.fullPath = child;
                t.size     = 0;
                tasks.push_back(t);
            }
            else
            {
                directFiles += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // ---- 并行扫描各子目录 ----
    if (!tasks.empty())
    {
        g_tasks     = tasks.data();
        g_taskCount = (LONG)tasks.size();
        g_nextIndex = 0;

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        LONG threads = (LONG)si.dwNumberOfProcessors;
        if (threads < 1) threads = 1;
        if (threads > 8) threads = 8;
        if (threads > g_taskCount) threads = g_taskCount;

        std::vector<HANDLE> hs;
        for (LONG i = 0; i < threads; i++)
        {
            HANDLE t = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
            if (t) hs.push_back(t);
        }
        for (HANDLE t : hs)
        {
            WaitForSingleObject(t, INFINITE);
            CloseHandle(t);
        }
    }

    // ---- 汇总输出 ----
    g_out.reserve(tasks.size() * 96 + 128);

    unsigned long long total = directFiles;
    for (auto& t : tasks)
    {
        total += t.size;
        AppendLine(t.size, t.name, t.fullPath);
    }

    // 目标目录里直接躺着的文件 (不属于任何子目录)
    if (directFiles > 0)
        AppendLine(directFiles, L"(根目录下的文件)", root);

    char num[32];
    sprintf(num, "%llu", total);
    g_out += "TOTAL\t";
    g_out += num;
    g_out += '\t';
    AppendUtf8(root);
    g_out += '\n';

    // 第二个参数可选: 输出文件路径 (避开管道缓冲区死锁)
    if (argc >= 3)
    {
        HANDLE fh = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            WriteFile(fh, g_out.data(), (DWORD)g_out.size(), &wrote, NULL);
            CloseHandle(fh);
            return 0;
        }
        fwprintf(stderr, L"cannot write: %ls\n", argv[2]);
        return 3;
    }

    fwrite(g_out.data(), 1, g_out.size(), stdout);
    fflush(stdout);
    return 0;
}