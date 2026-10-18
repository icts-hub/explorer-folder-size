// test.cpp - FolderSizeExt 集成测试
// 两种模式:
//   test.exe <dll路径> <目录>    直接 LoadLibrary + DllGetClassObject (验证 DLL 本身)
//   test.exe --com     <目录>    通过注册表 CoCreateInstance (验证安装是否真的生效)
//
// 输出走 WriteConsoleW, 避免 wprintf 在 C locale 下把中文变成 '?'

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
#include <cstdio>
#include <cstdarg>
#include <string>

typedef HRESULT (STDMETHODCALLTYPE *PFN_GetClassObject)(REFCLSID, REFIID, void**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_CanUnloadNow)(void);

// {8F3E2A71-4C9B-4E2D-9A7B-3C1D5E8F0A42}
static const GUID CLSID_FolderSizeColumn =
    { 0x8F3E2A71, 0x4C9B, 0x4E2D, { 0x9A, 0x7B, 0x3C, 0x1D, 0x5E, 0x8F, 0x0A, 0x42 } };

static int g_fail = 0;
static bool g_console = false;

static void WOut(const wchar_t* s)
{
    if (g_console)
    {
        DWORD written = 0;
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)wcslen(s), &written, nullptr);
    }
    else
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1)
        {
            std::string b((size_t)(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, s, -1, &b[0], n, nullptr, nullptr);
            fwrite(b.data(), 1, b.size(), stdout);
        }
    }
}

static void WPrintf(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    if (n < 0) n = 2047;
    buf[n] = L'\0';
    WOut(buf);
}

static void Check(bool cond, const wchar_t* what)
{
    WPrintf(L"  [%s] %ls\n", cond ? L"PASS" : L"FAIL", what);
    if (!cond) g_fail++;
}

static std::wstring VariantText(VARIANT* v)
{
    if (v->vt == VT_BSTR && v->bstrVal) return v->bstrVal;
    return L"<none>";
}

// 拿到 IColumnProvider 后跑同一套断言
static int RunChecks(IColumnProvider* cp, const wchar_t* target)
{
    SHCOLUMNINIT sci = { 0 };
    sci.dwFlags = 0;
    lstrcpynW(sci.wszFolder, target, MAX_PATH);
    HRESULT hr = cp->Initialize(&sci);
    Check(SUCCEEDED(hr), L"Initialize");

    SHCOLUMNINFO ci = { 0 };
    hr = cp->GetColumnInfo(0, &ci);
    WPrintf(L"       列标题: %ls\n", ci.wszTitle);
    WPrintf(L"       vt=%u fmt=%lu cChars=%u csFlags=0x%lX\n",
            ci.vt, ci.fmt, ci.cChars, ci.csFlags);
    Check(hr == S_OK, L"GetColumnInfo(0) 返回 S_OK");
    Check(ci.vt == VT_BSTR, L"列类型为 VT_BSTR");
    Check(wcslen(ci.wszTitle) > 0, L"列标题非空");

    hr = cp->GetColumnInfo(1, nullptr);
    Check(hr == S_FALSE, L"GetColumnInfo(1) 返回 S_FALSE (只有一列)");

    SHCOLUMNDATA scd = { 0 };
    scd.dwFlags = 0;
    scd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    scd.pwszExt = nullptr;
    lstrcpynW(scd.wszFile, target, MAX_PATH);

    VARIANT var;
    VariantInit(&var);
    hr = cp->GetItemData(&ci.scid, &scd, &var);
    Check(hr == S_OK, L"GetItemData 对文件夹返回 S_OK");
    WPrintf(L"       首次返回: %ls\n", VariantText(&var).c_str());
    VariantClear(&var);

    bool gotReal = false;
    std::wstring value;
    for (int i = 0; i < 120; i++)
    {
        Sleep(500);
        VariantInit(&var);
        hr = cp->GetItemData(&ci.scid, &scd, &var);
        value = VariantText(&var);
        VariantClear(&var);
        if (SUCCEEDED(hr) && value != L"\u2026" && value != L"<none>")
        {
            gotReal = true;
            break;
        }
    }
    WPrintf(L"       后台算完: %ls\n", value.c_str());
    Check(gotReal, L"后台计算完成并返回真实大小");

    SHCOLUMNDATA scdFile = { 0 };
    scdFile.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    lstrcpynW(scdFile.wszFile, L"C:\\Windows\\notepad.exe", MAX_PATH);

    VariantInit(&var);
    hr = cp->GetItemData(&ci.scid, &scdFile, &var);
    Check(hr == S_FALSE, L"GetItemData 对文件返回 S_FALSE (交给系统原生大小列)");
    VariantClear(&var);

    SHCOLUMNID wrongScid = ci.scid;
    wrongScid.pid = 999;
    VariantInit(&var);
    hr = cp->GetItemData(&wrongScid, &scd, &var);
    Check(hr == S_FALSE, L"错误的 SCID 返回 S_FALSE");
    VariantClear(&var);

    VariantInit(&var);
    cp->GetItemData(&ci.scid, &scd, &var);
    std::wstring again = VariantText(&var);
    VariantClear(&var);
    Check(again == value, L"缓存命中, 两次结果一致");
    WPrintf(L"       缓存复读: %ls\n", again.c_str());

    return g_fail;
}

int wmain(int argc, wchar_t** argv)
{
    // 有控制台 -> WriteConsoleW; 被重定向 -> UTF-8 写文件
    DWORD mode = 0;
    g_console = GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    if (g_console)
    {
        SetConsoleOutputCP(CP_UTF8);
        CONSOLE_FONT_INFOEX fi = { 0 };
        (void)fi;
    }

    bool useCom = (argc > 1 && wcscmp(argv[1], L"--com") == 0);

    const wchar_t* dllPath = useCom ? L"(通过注册表)" : ((argc > 1) ? argv[1] : L"FolderSizeExt.dll");
    const wchar_t* target  = (argc > 2) ? argv[2] : L"C:\\Windows";

    WPrintf(L"=== FolderSizeExt 集成测试 ===\n");
    WPrintf(L"模式   : %ls\n", useCom ? L"CoCreateInstance (验证安装)" : L"LoadLibrary (验证 DLL)");
    WPrintf(L"来源   : %ls\n", dllPath);
    WPrintf(L"目标   : %ls\n\n", target);

    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrInit)) { WPrintf(L"FAIL: CoInitializeEx 0x%08lX\n", hrInit); return 1; }

    IColumnProvider* cp = nullptr;
    IClassFactory*   cf = nullptr;

    if (useCom)
    {
        HRESULT hr = CoCreateInstance(CLSID_FolderSizeColumn, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_IColumnProvider,
                                      (void**)&cp);
        WPrintf(L"       CoCreateInstance -> 0x%08lX\n", hr);
        Check(SUCCEEDED(hr) && cp != nullptr, L"经注册表激活 IColumnProvider 成功");
        if (!cp) { CoUninitialize(); WPrintf(L"\n=== 失败 ===\n"); return 1; }
        int rc = RunChecks(cp, target);
        cp->Release();
        CoUninitialize();
        WPrintf(L"\n=== 结果: %s (%d 项失败) ===\n", rc ? L"失败" : L"全部通过", rc);
        return rc ? 1 : 0;
    }

    HMODULE h = LoadLibraryW(dllPath);
    if (!h) { WPrintf(L"FAIL: LoadLibrary 失败, GetLastError=%lu\n", GetLastError()); return 1; }
    Check(true, L"LoadLibrary");

    PFN_GetClassObject pGetClassObject =
        (PFN_GetClassObject)(void*)GetProcAddress(h, "DllGetClassObject");
    PFN_CanUnloadNow pCanUnloadNow =
        (PFN_CanUnloadNow)(void*)GetProcAddress(h, "DllCanUnloadNow");

    Check(pGetClassObject != nullptr, L"导出 DllGetClassObject");
    Check(pCanUnloadNow != nullptr, L"导出 DllCanUnloadNow");
    if (!pGetClassObject) return 1;

    HRESULT hr = pGetClassObject(CLSID_FolderSizeColumn, IID_IClassFactory, (void**)&cf);
    Check(SUCCEEDED(hr) && cf != nullptr, L"DllGetClassObject 返回 IClassFactory");
    if (!cf) return 1;

    IClassFactory* cfBad = nullptr;
    GUID notMine = CLSID_FolderSizeColumn;
    notMine.Data1 = 0xDEADBEEF;
    hr = pGetClassObject(notMine, IID_IClassFactory, (void**)&cfBad);
    Check(hr == CLASS_E_CLASSNOTAVAILABLE, L"未知 CLSID 返回 CLASS_E_CLASSNOTAVAILABLE");

    hr = cf->CreateInstance(nullptr, IID_IColumnProvider, (void**)&cp);
    Check(SUCCEEDED(hr) && cp != nullptr, L"CreateInstance(IID_IColumnProvider)");
    if (!cp) return 1;

    int rc = RunChecks(cp, target);

    cp->Release();
    cf->Release();
    // 故意不 FreeLibrary: 后台线程仍在运行

    CoUninitialize();
    WPrintf(L"\n=== 结果: %s (%d 项失败) ===\n", rc ? L"失败" : L"全部通过", rc);
    return rc ? 1 : 0;
}
