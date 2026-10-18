// FolderSizeExt - 资源管理器"文件夹大小"列扩展
// 编译: g++ -shared -std=c++17 (见 build.bat)
//
// 设计要点:
//  1. 绝不在 UI 线程做递归扫描 —— GetItemData 只读内存缓存, 未命中就丢给后台线程
//  2. 所有 COM 入口都包 try/catch, 异常绝不跨越边界传回 Explorer
//  3. 只用 Win32 原语 (CRITICAL_SECTION / CONDITION_VARIABLE / CreateThread),
//     避开 MinGW "win32" 线程模型下 std::thread / std::mutex 的兼容问题
//  4. 大小缓存持久化到 %LOCALAPPDATA%\FolderSizeExt\cache.dat

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
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <map>
#include <deque>
#include <vector>
#include <new>
#include <cstdio>
#include <cstring>

// ---------------- CLSID / SCID ----------------
// {8F3E2A71-4C9B-4E2D-9A7B-3C1D5E8F0A42}
static const GUID CLSID_FolderSizeColumn =
    { 0x8F3E2A71, 0x4C9B, 0x4E2D, { 0x9A, 0x7B, 0x3C, 0x1D, 0x5E, 0x8F, 0x0A, 0x42 } };

// 自定义属性键, 避免和系统"大小"属性冲突
static const GUID FMTID_FolderSize =
    { 0x6B1F2C44, 0x8D3A, 0x4B77, { 0xA2, 0x19, 0x4C, 0x7E, 0x63, 0x0B, 0xD5, 0x91 } };
static const DWORD PID_FOLDER_SIZE = 2;

#ifndef SHCOLSTATE_TYPE_STR
#define SHCOLSTATE_TYPE_STR   0x1
#define SHCOLSTATE_ONBYDEFAULT 0x10
#define SHCOLSTATE_SLOW       0x20
#endif
#ifndef FIND_FIRST_EX_LARGE_FETCH
#define FIND_FIRST_EX_LARGE_FETCH 0x2
#endif

static HINSTANCE g_hInst = NULL;
static CRITICAL_SECTION g_cs;
static bool g_csReady = false;
static CONDITION_VARIABLE g_cv;

static std::map<std::wstring, unsigned long long> g_cache;  // 绝对路径 -> 字节数
static std::map<std::wstring, int> g_inflight;              // 防止重复排队
static std::deque<std::wstring> g_queue;
static bool g_workerStarted = false;
static HANDLE g_worker = NULL;
static bool g_cacheDirty = false;

// ---------------- 工具函数 ----------------

static void FormatSize(unsigned long long n, wchar_t* out, size_t cch)
{
    if (n < 1024ULL)
        swprintf(out, cch, L"%llu B", n);
    else if (n < 1024ULL * 1024ULL)
        swprintf(out, cch, L"%.1f KB", (double)n / 1024.0);
    else if (n < 1024ULL * 1024ULL * 1024ULL)
        swprintf(out, cch, L"%.1f MB", (double)n / 1048576.0);
    else if (n < 1024ULL * 1024ULL * 1024ULL * 1024ULL)
        swprintf(out, cch, L"%.2f GB", (double)n / 1073741824.0);
    else
        swprintf(out, cch, L"%.2f TB", (double)n / 1099511627776.0);
}

// 递归求目录总大小。跳过重解析点(符号链接/联接点)防止死循环。
static unsigned long long DirSize(const std::wstring& root)
{
    unsigned long long total = 0;
    std::vector<std::wstring> stack;
    stack.push_back(root);

    while (!stack.empty())
    {
        std::wstring dir = stack.back();
        stack.pop_back();

        std::wstring pattern = dir;
        if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
        pattern += L'*';

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            // 跳过联接点/符号链接, 否则会重复计算或陷入循环
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            std::wstring child = dir;
            if (!child.empty() && child.back() != L'\\') child += L'\\';
            child += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
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

// ---------------- 缓存持久化 ----------------

static std::wstring CacheDir()
{
    wchar_t buf[MAX_PATH] = { 0 };
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf)))
        return std::wstring();
    std::wstring d = buf;
    d += L"\\FolderSizeExt";
    CreateDirectoryW(d.c_str(), NULL);
    return d;
}

static std::wstring CacheFile()
{
    std::wstring d = CacheDir();
    if (d.empty()) return std::wstring();
    return d + L"\\cache.dat";
}

static void LoadCache()
{
    std::wstring f = CacheFile();
    if (f.empty()) return;

    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD magic = 0, count = 0, got = 0;
    if (ReadFile(h, &magic, 4, &got, NULL) && got == 4 && magic == 0x46534B31) // 'FSK1'
    {
        if (ReadFile(h, &count, 4, &got, NULL) && got == 4)
        {
            for (DWORD i = 0; i < count; i++)
            {
                DWORD chars = 0;
                if (!ReadFile(h, &chars, 4, &got, NULL) || got != 4) break;
                if (chars == 0 || chars > 32768) break;

                std::vector<wchar_t> pw(chars);
                if (!ReadFile(h, pw.data(), chars * sizeof(wchar_t), &got, NULL)) break;
                if (got != chars * sizeof(wchar_t)) break;

                unsigned long long sz = 0;
                if (!ReadFile(h, &sz, 8, &got, NULL) || got != 8) break;

                g_cache[std::wstring(pw.data(), chars)] = sz;
            }
        }
    }
    CloseHandle(h);
}

static void SaveCache()
{
    std::wstring f = CacheFile();
    if (f.empty()) return;

    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD magic = 0x46534B31;
    DWORD count = (DWORD)g_cache.size();
    DWORD wrote = 0;
    WriteFile(h, &magic, 4, &wrote, NULL);
    WriteFile(h, &count, 4, &wrote, NULL);

    for (auto& kv : g_cache)
    {
        DWORD chars = (DWORD)kv.first.size();
        if (chars > 32768) continue;
        WriteFile(h, &chars, 4, &wrote, NULL);
        WriteFile(h, kv.first.data(), chars * sizeof(wchar_t), &wrote, NULL);
        unsigned long long sz = kv.second;
        WriteFile(h, &sz, 8, &wrote, NULL);
    }
    CloseHandle(h);
}

// ---------------- 后台工作线程 ----------------

static DWORD WINAPI WorkerProc(LPVOID)
{
    for (;;)
    {
        std::wstring task;

        EnterCriticalSection(&g_cs);
        while (g_queue.empty())
        {
            // 队列排空 = 这一批算完了, 立刻落盘, 下次打开资源管理器可直接命中
            if (g_cacheDirty)
            {
                g_cacheDirty = false;
                LeaveCriticalSection(&g_cs);
                SaveCache();
                EnterCriticalSection(&g_cs);
                if (!g_queue.empty()) break;
            }
            SleepConditionVariableCS(&g_cv, &g_cs, 5000);
        }
        if (g_queue.empty()) { LeaveCriticalSection(&g_cs); continue; }

        task = g_queue.front();
        g_queue.pop_front();
        LeaveCriticalSection(&g_cs);

        unsigned long long sz = DirSize(task);

        EnterCriticalSection(&g_cs);
        g_cache[task] = sz;
        g_inflight.erase(task);
        g_cacheDirty = true;
        LeaveCriticalSection(&g_cs);

        // 让资源管理器刷新该条目 (节流到最多约 5 次/秒, 避免刷屏)
        static DWORD lastNotify = 0;
        DWORD now = GetTickCount();
        if ((now - lastNotify) > 200)
        {
            lastNotify = now;
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, task.c_str(), NULL);
        }
    }
    return 0;
}

static void EnsureWorker()
{
    if (g_workerStarted) return;
    g_workerStarted = true;
    g_worker = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
    if (g_worker) SetThreadPriority(g_worker, THREAD_PRIORITY_BELOW_NORMAL);
}

static void Enqueue(const std::wstring& path)
{
    EnterCriticalSection(&g_cs);
    if (g_cache.find(path) == g_cache.end() && g_inflight.find(path) == g_inflight.end())
    {
        g_inflight[path] = 1;
        g_queue.push_back(path);
        WakeAllConditionVariable(&g_cv);
    }
    LeaveCriticalSection(&g_cs);
}

// ---------------- IColumnProvider 实现 ----------------

class FolderSizeColumn : public IColumnProvider
{
public:
    FolderSizeColumn() : m_ref(1) {}
    virtual ~FolderSizeColumn() {}

    // ---- IUnknown ----
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IColumnProvider))
        {
            *ppv = static_cast<IColumnProvider*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // ---- IColumnProvider ----
    STDMETHODIMP Initialize(LPCSHCOLUMNINIT psci) override
    {
        try
        {
            if (psci && psci->wszFolder[0])
                m_folder = psci->wszFolder;
        }
        catch (...) {}
        return S_OK;
    }

    STDMETHODIMP GetColumnInfo(DWORD dwIndex, SHCOLUMNINFO* psci) override
    {
        try
        {
            if (dwIndex != 0) return S_FALSE;
            if (!psci) return E_INVALIDARG;

            ZeroMemory(psci, sizeof(*psci));
            psci->scid.fmtid = FMTID_FolderSize;
            psci->scid.pid   = PID_FOLDER_SIZE;
            psci->vt         = VT_BSTR;
            psci->fmt        = LVCFMT_RIGHT;
            psci->cChars     = 10;
            psci->csFlags    = SHCOLSTATE_TYPE_STR | SHCOLSTATE_SLOW;

            lstrcpynW(psci->wszTitle, L"文件夹大小", MAX_COLUMN_NAME_LEN);
            lstrcpynW(psci->wszDescription, L"文件夹内所有文件的总大小（后台计算）",
                      MAX_COLUMN_DESC_LEN);
            return S_OK;
        }
        catch (...) { return E_FAIL; }
    }

    STDMETHODIMP GetItemData(LPCSHCOLUMNID pscid, LPCSHCOLUMNDATA pscd, VARIANT* pvarData) override
    {
        try
        {
            if (!pscid || !pscd || !pvarData) return E_INVALIDARG;

            VariantInit(pvarData);

            // 只认自己的那一列
            if (!IsEqualGUID(pscid->fmtid, FMTID_FolderSize) ||
                pscid->pid != PID_FOLDER_SIZE)
                return S_FALSE;

            // 文件交给系统原生的"大小"列, 这里只处理文件夹
            if (!(pscd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                return S_FALSE;

            std::wstring path = pscd->wszFile;
            if (path.empty()) return S_FALSE;

            // wszFile 通常是完整路径; 但为保险起见, 若不存在则拼上当前目录
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                if (!m_folder.empty())
                {
                    std::wstring alt = m_folder;
                    if (alt.back() != L'\\') alt += L'\\';
                    alt += path;
                    if (GetFileAttributesW(alt.c_str()) != INVALID_FILE_ATTRIBUTES)
                        path = alt;
                }
            }

            // 去掉结尾反斜杠, 保证缓存 key 一致
            while (path.size() > 3 && path.back() == L'\\')
                path.pop_back();

            unsigned long long sz = 0;
            bool hit = false;

            EnterCriticalSection(&g_cs);
            auto it = g_cache.find(path);
            if (it != g_cache.end()) { sz = it->second; hit = true; }
            LeaveCriticalSection(&g_cs);

            if (!hit)
            {
                EnsureWorker();
                Enqueue(path);
                // 给个占位符, 算完后 SHChangeNotify 会触发刷新
                pvarData->vt      = VT_BSTR;
                pvarData->bstrVal = SysAllocString(L"\u2026"); // …
                if (!pvarData->bstrVal) VariantInit(pvarData);
                return S_OK;
            }

            wchar_t buf[64] = { 0 };
            FormatSize(sz, buf, 64);

            pvarData->vt      = VT_BSTR;
            pvarData->bstrVal = SysAllocString(buf);
            if (!pvarData->bstrVal) { VariantInit(pvarData); return E_OUTOFMEMORY; }
            return S_OK;
        }
        catch (...)
        {
            return S_FALSE;
        }
    }

private:
    LONG         m_ref;
    std::wstring m_folder;
};

// ---------------- 类厂 ----------------

class ClassFactory : public IClassFactory
{
public:
    ClassFactory() : m_ref(1) {}
    virtual ~ClassFactory() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return (ULONG)InterlockedIncrement(&m_ref);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        FolderSizeColumn* obj = new (std::nothrow) FolderSizeColumn();
        if (!obj) return E_OUTOFMEMORY;

        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    LONG m_ref;
};

// ---------------- DLL 导出 ----------------

extern "C" __declspec(dllexport) HRESULT STDMETHODCALLTYPE
DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, CLSID_FolderSizeColumn)) return CLASS_E_CLASSNOTAVAILABLE;

    ClassFactory* cf = new (std::nothrow) ClassFactory();
    if (!cf) return E_OUTOFMEMORY;

    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

extern "C" __declspec(dllexport) HRESULT STDMETHODCALLTYPE DllCanUnloadNow(void)
{
    // 后台线程持有代码, 永不卸载
    return S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
        InitializeCriticalSection(&g_cs);
        InitializeConditionVariable(&g_cv);
        g_csReady = true;
        LoadCache();
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        if (g_csReady)
        {
            EnterCriticalSection(&g_cs);
            bool dirty = g_cacheDirty;
            g_cacheDirty = false;
            LeaveCriticalSection(&g_cs);
            if (dirty) SaveCache();
        }
    }
    return TRUE;
}
