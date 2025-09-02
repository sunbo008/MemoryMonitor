#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <cstring>  // for memset
#include <tlhelp32.h>  // for process enumeration
#include <fstream>     // for debug logging

#include <windowsx.h>
#include <iomanip>
#include <algorithm>

// 确保使用Unicode编码
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

struct Allocation {
    void* ptr;
    size_t size;
    bool freed;
};

std::vector<Allocation> g_allocations;
// 使用预分配的vector提高性能
std::vector<size_t> g_memHistory, g_peakHistory;
std::vector<time_t> g_timeHistory;
// 缓存上次的最大值，避免重复计算
static size_t g_lastMaxVal = 1;
static bool g_needRecalcMax = true;

HWND hEdit, hButton, hList, hComboProcess;
int g_mouseX = -1, g_mouseY = -1;
bool g_mouseInGraph = false;
HFONT g_hFontSimSun = NULL;
// 添加防抖动变量
static DWORD g_lastMouseMove = 0;
static const DWORD MOUSE_THROTTLE_MS = 50; // 50ms防抖动

// 进程监控相关变量
static DWORD g_targetProcessId = 0;  // 0表示监控当前进程
static HANDLE g_targetProcessHandle = NULL;

// 获取程序同目录下的日志文件路径
std::wstring getLogFilePath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    
    // 获取目录路径
    std::wstring dir = exePath;
    size_t lastSlash = dir.find_last_of(L"\\");
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash + 1);
    }
    
    return dir + L"debug.log";
}

// 调试日志函数 - 写入到程序同目录
void writeDebugLog(const std::wstring& message) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::wstringstream ss;
    ss << L"[" << st.wHour << L":" << st.wMinute << L":" << st.wSecond << L"." << st.wMilliseconds << L"] " << message;
    
    // 输出到调试器
    OutputDebugStringW(ss.str().c_str());
    
    // 写入到程序同目录下的debug.log
    std::wstring logPath = getLogFilePath();
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
        std::string utf8Message;
        int len = WideCharToMultiByte(CP_UTF8, 0, ss.str().c_str(), -1, NULL, 0, NULL, NULL);
        if (len > 0) {
            utf8Message.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, ss.str().c_str(), -1, &utf8Message[0], len, NULL, NULL);
            utf8Message += "\r\n";
            DWORD written;
            WriteFile(hFile, utf8Message.c_str(), (DWORD)utf8Message.length(), &written, NULL);
        }
        CloseHandle(hFile);
    }
}

static std::wstring formatBytes(size_t bytes) {
    double v = (double)bytes;
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    std::wstringstream ss;
    if (v < 10.0)
        ss << std::fixed << std::setprecision(1) << v << L" " << units[i];
    else
        ss << std::fixed << std::setprecision(0) << v << L" " << units[i];
    return ss.str();
}
size_t getCurrentRSS() {
    PROCESS_MEMORY_COUNTERS counters;
    HANDLE hProcess = (g_targetProcessHandle != NULL) ? g_targetProcessHandle : GetCurrentProcess();
    if (GetProcessMemoryInfo(hProcess, &counters, sizeof(counters))) {
        return counters.WorkingSetSize;
    }
    return 0;
}
size_t getPeakRSS() {
    PROCESS_MEMORY_COUNTERS counters;
    HANDLE hProcess = (g_targetProcessHandle != NULL) ? g_targetProcessHandle : GetCurrentProcess();
    if (GetProcessMemoryInfo(hProcess, &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

void addListItem(HWND list, size_t size, int index) {
    std::wstringstream ss;
    ss << L"内存块 " << index << L" | " << size / 1024 << L" KB";
    // 插入后滚动到新项
    int listIndex = (int)SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
    if (listIndex >= 0) {
        SendMessageW(list, LB_SETTOPINDEX, (WPARAM)listIndex, 0);
    }
}

// 根据项目数量动态调整 ComboBox 下拉高度，超过20项则限制可见20项并出现滚动条
void adjustComboDropHeight(HWND hCombo) {
    if (!hCombo || !IsWindow(hCombo)) return;
    int totalItems = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
    int visible = totalItems <= 0 ? 4 : totalItems; // 至少显示4行
    if (visible > 20) visible = 20;

    int itemHeight = (int)SendMessageW(hCombo, CB_GETITEMHEIGHT, 0, 0);
    int selHeight = (int)SendMessageW(hCombo, CB_GETITEMHEIGHT, (WPARAM)-1, 0);
    if (itemHeight <= 0) itemHeight = 16;
    if (selHeight <= 0) selHeight = 24;

    int borderPad = GetSystemMetrics(SM_CYEDGE) * 6; // 边框/阴影余量
    int newHeight = selHeight + itemHeight * visible + borderPad;

    RECT r; GetWindowRect(hCombo, &r);
    POINT pt = { r.left, r.top };
    HWND hParent = GetParent(hCombo);
    if (!hParent) return;
    ScreenToClient(hParent, &pt);
    int width = r.right - r.left;

    SetWindowPos(hCombo, NULL, pt.x, pt.y, width, newHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

// 填充进程列表
void populateProcessList(HWND hCombo) {
    writeDebugLog(L"Starting to populate process list...");
    
    if (!hCombo || !IsWindow(hCombo)) {
        writeDebugLog(L"ERROR: Invalid ComboBox handle!");
        return;
    }
    
    int resetResult = (int)SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    writeDebugLog(L"CB_RESETCONTENT result: " + std::to_wstring(resetResult));
    
    // 添加当前进程选项
    std::wstringstream currentProcess;
    currentProcess << L"[当前进程] " << GetCurrentProcessId();
    int addResult = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)currentProcess.str().c_str());
    writeDebugLog(L"Added current process, index: " + std::to_wstring(addResult));
    SendMessageW(hCombo, CB_SETITEMDATA, 0, (LPARAM)GetCurrentProcessId());
    
    // 枚举系统进程（不再限制数量，自动通过下拉高度与滚动条处理）
    struct ProcItem { std::wstring name; DWORD pid; };
    std::vector<ProcItem> procItems;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        writeDebugLog(L"Process snapshot created successfully");
        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(pe32);
        
        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                // 跳过系统进程和当前进程
                if (pe32.th32ProcessID != 0 && pe32.th32ProcessID != 4 && 
                    pe32.th32ProcessID != GetCurrentProcessId()) {
                    
                    // 尝试打开进程以检查是否有权限访问
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe32.th32ProcessID);
                    if (hProc) {
                        procItems.push_back(ProcItem{ std::wstring(pe32.szExeFile), pe32.th32ProcessID });
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(hSnapshot, &pe32));
        } else {
            writeDebugLog(L"Process32FirstW failed");
        }
        CloseHandle(hSnapshot);
    } else {
        writeDebugLog(L"CreateToolhelp32Snapshot failed");
    }

    // 名称升序排序（不区分大小写）
    std::sort(procItems.begin(), procItems.end(), [](const ProcItem& a, const ProcItem& b){
        int r = CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE);
        return r == CSTR_LESS_THAN; // 若相等则保持稳定顺序
    });

    // 添加已排序的进程到下拉框
    int logCount = 0;
    for (const auto& it : procItems) {
        std::wstringstream ss;
        ss << it.name << L" (PID: " << it.pid << L")";
        int index = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
        SendMessageW(hCombo, CB_SETITEMDATA, index, (LPARAM)it.pid);
        if (logCount < 5) {
            writeDebugLog(L"Added process: " + it.name + L" at index " + std::to_wstring(index));
            ++logCount;
        }
    }

    writeDebugLog(L"Total processes added: " + std::to_wstring((int)procItems.size() + 1)); // +1 for current进程
    
    // 获取ComboBox项目总数
    int totalItems = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
    writeDebugLog(L"ComboBox total items: " + std::to_wstring(totalItems));
    
    // 默认选择当前进程
    int selResult = (int)SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    writeDebugLog(L"CB_SETCURSEL result: " + std::to_wstring(selResult));

    // 根据数量动态调整下拉高度
    adjustComboDropHeight(hCombo);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        // 进程选择ComboBox - 使用CBS_DROPDOWN样式，允许超过可见项时显示滚动条
        writeDebugLog(L"Creating ComboBox...");
        hComboProcess = CreateWindowW(L"COMBOBOX", NULL, 
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_HASSTRINGS | CBS_AUTOHSCROLL | WS_VSCROLL,
            10, height / 2 + 10, 250, 150, hwnd, (HMENU)4, NULL, NULL);
        
        if (hComboProcess) {
            writeDebugLog(L"ComboBox created successfully, handle: " + std::to_wstring((uintptr_t)hComboProcess));
        } else {
            writeDebugLog(L"ComboBox creation FAILED!");
        }

        hEdit = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER,
            270, height / 2 + 10, 100, 25, hwnd, (HMENU)1, NULL, NULL);

        hButton = CreateWindowW(L"BUTTON", L"申请内存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            380, height / 2 + 10, 120, 30, hwnd, (HMENU)2, NULL, NULL);

        hList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            10, height / 2 + 50, width - 20, height / 2 - 60, hwnd, (HMENU)3, NULL, NULL);

        // 减少定时器间隔，提高数据刷新频率
        SetTimer(hwnd, 1, 1000, NULL);
        // 预分配容器大小，减少内存重新分配
        g_memHistory.reserve(250);
        g_peakHistory.reserve(250);
        g_timeHistory.reserve(250);
        // 创建宋体并应用到控件
        g_hFontSimSun = CreateFontW(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FF_DONTCARE,L"宋体");
        if (g_hFontSimSun) {
            SendMessage(hComboProcess, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hEdit, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hButton, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
        }
                 // 设置列表框行高，确保文字完整显示
         SendMessageW(hList, LB_SETITEMHEIGHT, 0, 20);
        
        // 立即收集初始数据，避免启动时空白
        for (int i = 0; i < 3; ++i) {
            g_memHistory.push_back(getCurrentRSS());
            g_peakHistory.push_back(getPeakRSS());
            g_timeHistory.push_back(time(NULL));
        }
        g_needRecalcMax = true;
        
        // 填充进程列表
        writeDebugLog(L"About to populate process list...");
        populateProcessList(hComboProcess);
        // 初次根据数量调整下拉高度
        adjustComboDropHeight(hComboProcess);
        writeDebugLog(L"Process list population completed");
        
        // 立即刷新一次显示
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_SIZE: {
        // 窗口大小改变时重新布局控件
        if (wParam != SIZE_MINIMIZED && hComboProcess && hEdit && hButton && hList) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;
            
            // 重新定位控件，保持相对位置
            int midY = height / 2;
            
            // 仅定位，不改变大小；大小（含下拉高度）由 adjustComboDropHeight 控制
            SetWindowPos(hComboProcess, NULL, 10, midY + 10, 0, 0,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);

            // 根据项目数量动态调整下拉高度
            adjustComboDropHeight(hComboProcess);
            
            // 输入框位置
            SetWindowPos(hEdit, NULL, 270, midY + 10, 100, 25, 
                SWP_NOZORDER | SWP_NOACTIVATE);
            
            // 按钮位置
            SetWindowPos(hButton, NULL, 380, midY + 10, 120, 30, 
                SWP_NOZORDER | SWP_NOACTIVATE);
            
            // 列表框位置和大小，随窗口大小动态调整
            int listHeight = height / 2 - 60;
            if (listHeight < 100) listHeight = 100; // 最小高度
            SetWindowPos(hList, NULL, 10, midY + 50, width - 20, listHeight, 
                SWP_NOZORDER | SWP_NOACTIVATE);
            
            // 重绘整个窗口
            InvalidateRect(hwnd, NULL, TRUE);
        }
        break;
    }
    case WM_TIMER: {
        g_memHistory.push_back(getCurrentRSS());
        g_peakHistory.push_back(getPeakRSS());
        g_timeHistory.push_back(time(NULL));
        // 标记需要重新计算最大值
        g_needRecalcMax = true;
        // 使用更高效的方式管理历史数据，避免频繁的erase操作
        const size_t MAX_HISTORY = 200;
        if (g_memHistory.size() > MAX_HISTORY) {
            // 批量删除前面的一半数据，减少频繁的erase操作
            size_t removeCount = MAX_HISTORY / 4;
            g_memHistory.erase(g_memHistory.begin(), g_memHistory.begin() + removeCount);
            g_peakHistory.erase(g_peakHistory.begin(), g_peakHistory.begin() + removeCount);
            g_timeHistory.erase(g_timeHistory.begin(), g_timeHistory.begin() + removeCount);
            // 数据变化后需要重新计算最大值
            g_needRecalcMax = true;
        }
        // 只刷新图形区域，不刷新整个窗口
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT graphRect = {0, 0, rc.right, rc.bottom / 2};
        InvalidateRect(hwnd, &graphRect, FALSE);
        break;
    }

    case WM_LBUTTONDOWN: {
        // 记录鼠标点击位置
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        writeDebugLog(L"Mouse click at (" + std::to_wstring(x) + L", " + std::to_wstring(y) + L")");
        
        // 检查是否点击在ComboBox上
        if (hComboProcess) {
            RECT comboRect;
            GetWindowRect(hComboProcess, &comboRect);
            POINT pt = {x, y};
            ClientToScreen(hwnd, &pt);
            if (PtInRect(&comboRect, pt)) {
                writeDebugLog(L"Click detected on ComboBox!");
            }
        }
        break;
    }
    case WM_COMMAND: {
        // 添加更详细的命令消息日志
        WORD ctrlId = LOWORD(wParam);
        WORD notifyCode = HIWORD(wParam);
        writeDebugLog(L"WM_COMMAND: ctrlId=" + std::to_wstring(ctrlId) + L", notifyCode=" + std::to_wstring(notifyCode));
        
        if (ctrlId == 4) { // ComboBox ID
            writeDebugLog(L"ComboBox command received!");
            switch (notifyCode) {
            case CBN_DROPDOWN:
                writeDebugLog(L"CBN_DROPDOWN - ComboBox about to drop down");
                break;
            case CBN_CLOSEUP:
                writeDebugLog(L"CBN_CLOSEUP - ComboBox closed up");
                break;
            case CBN_SELCHANGE:
                writeDebugLog(L"CBN_SELCHANGE - Selection changed");
                break;
            case CBN_SETFOCUS:
                writeDebugLog(L"CBN_SETFOCUS - ComboBox got focus");
                break;
            case CBN_KILLFOCUS:
                writeDebugLog(L"CBN_KILLFOCUS - ComboBox lost focus");
                break;
            case CBN_ERRSPACE:
                writeDebugLog(L"CBN_ERRSPACE - ComboBox out of memory! Switching to CBS_DROPDOWN style should fix this.");
                // 强制重新创建ComboBox（如果需要的话）
                break;
            default:
                writeDebugLog(L"Other ComboBox notification: " + std::to_wstring(notifyCode));
                break;
            }
        }
        
        // 原有的命令处理逻辑
        if (LOWORD(wParam) == 2) { // 申请按钮
             wchar_t buf[64];
             GetWindowTextW(hEdit, buf, 64);
            int size = _wtoi(buf);
            if (size == 0) size = (rand() % 50 + 1) * 1024 * 1024; // 随机1-50MB
            char* p = new char[size];
            // 优化内存初始化，使用memset更高效
            memset(p, 'A' + (rand() % 26), size);

            Allocation a{ p, (size_t)size, false };
            g_allocations.push_back(a);
            addListItem(hList, size, (int)g_allocations.size() - 1);
        }
                 else if (LOWORD(wParam) == 3 && HIWORD(wParam) == LBN_DBLCLK) { // 双击列表释放
             int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel < (int)g_allocations.size()) {
                // 只有未释放的内存块才可以被双击释放
                if (!g_allocations[sel].freed) {
                    delete[] (char*)g_allocations[sel].ptr;
                    g_allocations[sel].freed = true;
                    
                    // 更新列表项文字，添加"[已释放]"标识
                    wchar_t buf[256];
                    SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)buf);
                    std::wstring s(buf);
                    s += L" [已释放]";
                    SendMessageW(hList, LB_DELETESTRING, sel, 0);
                    SendMessageW(hList, LB_INSERTSTRING, sel, (LPARAM)s.c_str());
                    
                    // 重绘该项，使其显示为灰色
                    RECT itemRect;
                    SendMessage(hList, LB_GETITEMRECT, sel, (LPARAM)&itemRect);
                    InvalidateRect(hList, &itemRect, FALSE);
                }
                // 已释放的项不响应双击事件
            }
        }
        else if (LOWORD(wParam) == 4 && HIWORD(wParam) == CBN_SELCHANGE) { // 进程选择变化
            int sel = (int)SendMessageW(hComboProcess, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                DWORD processId = (DWORD)SendMessageW(hComboProcess, CB_GETITEMDATA, sel, 0);
                
                // 关闭之前的进程句柄
                if (g_targetProcessHandle && g_targetProcessHandle != GetCurrentProcess()) {
                    CloseHandle(g_targetProcessHandle);
                    g_targetProcessHandle = NULL;
                }
                
                g_targetProcessId = processId;
                
                // 如果不是当前进程，打开目标进程
                if (processId != GetCurrentProcessId()) {
                    g_targetProcessHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
                    if (!g_targetProcessHandle) {
                        // 如果无法打开进程，回退到当前进程
                        MessageBoxW(hwnd, L"无法访问选定的进程，可能需要管理员权限", L"警告", MB_OK | MB_ICONWARNING);
                        SendMessageW(hComboProcess, CB_SETCURSEL, 0, 0); // 选回当前进程
                        g_targetProcessId = GetCurrentProcessId();
                    }
                } else {
                    g_targetProcessHandle = NULL; // 当前进程不需要句柄
                }
                
                // 清空历史数据，重新开始监控
                g_memHistory.clear();
                g_peakHistory.clear();
                g_timeHistory.clear();
                g_needRecalcMax = true;
                
                // 立即收集新进程的数据
                for (int i = 0; i < 3; ++i) {
                    g_memHistory.push_back(getCurrentRSS());
                    g_peakHistory.push_back(getPeakRSS());
                    g_timeHistory.push_back(time(NULL));
                }
                
                // 刷新显示
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        // 防抖动：限制鼠标移动事件的处理频率
        DWORD currentTime = GetTickCount();
        if (currentTime - g_lastMouseMove < MOUSE_THROTTLE_MS) {
            break;
        }
        g_lastMouseMove = currentTime;
        
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mid = (rc.bottom - rc.top) / 2;
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        if (my < mid) {
            // 只有当鼠标位置发生显著变化时才更新
            if (abs(g_mouseX - mx) > 5 || abs(g_mouseY - my) > 5) {
                g_mouseX = mx;
                g_mouseY = my;
                if (!g_mouseInGraph) {
                    // start tracking leave
                    TRACKMOUSEEVENT tme = { sizeof(tme) };
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    TrackMouseEvent(&tme);
                    g_mouseInGraph = true;
                }
                // 只刷新图形区域
                RECT graphRect = {0, 0, rc.right, mid};
                InvalidateRect(hwnd, &graphRect, FALSE);
            }
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_mouseInGraph = false;
        g_mouseX = g_mouseY = -1;
        // 只刷新图形区域
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT graphRect = {0, 0, rc.right, rc.bottom / 2};
        InvalidateRect(hwnd, &graphRect, FALSE);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    // Double buffering: create compatible memory DC
    HDC memDC = CreateCompatibleDC(hdc);
    RECT rr; GetClientRect(hwnd, &rr);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, rr.right - rr.left, rr.bottom - rr.top);
    HBITMAP oldBm = (HBITMAP)SelectObject(memDC, hbm);
    HDC drawDC = memDC;
    HFONT hOldFont = NULL;
    if (g_hFontSimSun) hOldFont = (HFONT)SelectObject(drawDC, g_hFontSimSun);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int mid = (rc.bottom - rc.top) / 2;

        // 背景（深色）
        HBRUSH bgBrush = CreateSolidBrush(RGB(24,24,24));
        FillRect(drawDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        // 绘制网格 - 动态调整图表区域
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(50,50,50));
        HPEN lightGrid = CreatePen(PS_SOLID, 1, RGB(40,40,40));
        SelectObject(drawDC, gridPen);
        int graphTop = 10;  // 上边距
        int graphBottom = mid - 15;  // 下边距
        int graphLeft = 50;  // 左边距，为Y轴标签留空间
        int graphRight = rc.right - 10;  // 右边距
        
        // 确保图表区域有效
        if (graphBottom <= graphTop) graphBottom = graphTop + 100;
        if (graphRight <= graphLeft) graphRight = graphLeft + 200;
        // horizontal grid
        int hLines = 8;
        for (int i = 0; i <= hLines; ++i) {
            int y = graphTop + i * (graphBottom - graphTop) / hLines;
            SelectObject(drawDC, (i % 2 == 0) ? lightGrid : gridPen);
            MoveToEx(drawDC, graphLeft, y, NULL);
            LineTo(drawDC, graphRight, y);
        }
        // vertical grid
        int vStep = 40;
        for (int x = graphLeft; x <= graphRight; x += vStep) {
            MoveToEx(drawDC, x, graphTop, NULL);
            LineTo(drawDC, x, graphBottom);
        }

        int count = (int)g_memHistory.size();
        if (count > 1) {
            HPEN hBlue = CreatePen(PS_SOLID, 2, RGB(0, 0, 255));
            HPEN hRed = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));

            // 缩放 - 使用缓存的最大值，只在必要时重新计算
            size_t maxVal = g_lastMaxVal;
            if (g_needRecalcMax) {
                maxVal = 1;
                for (size_t v : g_peakHistory) {
                    if (v > maxVal) maxVal = v;
                }
                g_lastMaxVal = maxVal;
                g_needRecalcMax = false;
            }

            // 绘制 Y 轴刻度和标签 (自动单位)
            // 使用已经选中的宋体字体
            for (int t = 0; t <= 4; t++) {
                double frac = t / 4.0;
                int y = graphBottom - (int)(frac * (graphBottom - graphTop));
                MoveToEx(drawDC, graphLeft - 6, y, NULL);
                LineTo(drawDC, graphLeft, y);
                // Label
                size_t val = (size_t)(maxVal * frac);
                std::wstringstream ss;
                ss << formatBytes(val);
                // 以浅色字体显示
                SetTextColor(drawDC, RGB(180,180,180));
                SetBkMode(drawDC, TRANSPARENT);
                TextOutW(drawDC, 5, y - 8, ss.str().c_str(), (int)ss.str().size());
            }

            // 蓝线：当前内存
            // 先构造点数组并填充下面的区域
            std::vector<POINT> pts;
            pts.reserve(count + 2);
            int graphWidth = graphRight - graphLeft;
            for (int i = 0; i < count; ++i) {
                // 动态计算X坐标，根据图表宽度和数据点数量
                int x = graphLeft + (count > 1 ? (i * graphWidth) / (count - 1) : graphWidth / 2);
                int y = graphBottom - (int)((double)g_memHistory[i] / maxVal * (graphBottom - graphTop));
                pts.push_back({x, y});
            }
            // 填充多边形（曲线 + 底边回到起点）
            std::vector<POINT> poly;
            poly.reserve(pts.size() + 2);
            for (auto &p : pts) poly.push_back(p);
            // baseline at graphBottom
            poly.push_back({ pts.back().x, graphBottom });
            poly.push_back({ pts.front().x, graphBottom });
            // 半透明填充
            HBRUSH fillBrush = CreateSolidBrush(RGB(24,100,150));
            // 为半透明效果，使用 AlphaBlend requires compatible bitmap; as simpler approach, blend by creating pattern
            // We'll use a solid fill with lower contrast color
            SetDCPenColor(drawDC, RGB(24,100,150));
            HBRUSH hOldBrush = (HBRUSH)SelectObject(drawDC, fillBrush);
            Polygon(drawDC, poly.data(), (int)poly.size());
            SelectObject(drawDC, hOldBrush);
            DeleteObject(fillBrush);
            // 绘制折线顶边
            SelectObject(drawDC, hBlue);
            MoveToEx(drawDC, pts[0].x, pts[0].y, NULL);
            for (size_t i = 1; i < pts.size(); ++i) LineTo(drawDC, pts[i].x, pts[i].y);

            // 红线：峰值
            SelectObject(drawDC, hRed);
            if (count > 0) {
                int x0 = graphLeft;
                int y0 = graphBottom - (int)((double)g_peakHistory[0] / maxVal * (graphBottom - graphTop));
                MoveToEx(drawDC, x0, y0, NULL);
                for (int i = 1; i < count; i++) {
                    int x = graphLeft + (count > 1 ? (i * graphWidth) / (count - 1) : graphWidth / 2);
                    int y = graphBottom - (int)((double)g_peakHistory[i] / maxVal * (graphBottom - graphTop));
                    LineTo(drawDC, x, y);
                }
            }

            // 绘制图例 - 调整位置和大小确保文字完整显示
            int lx = rc.right - 320; // 增加左边距
            int ly = 10;
            HPEN lgBlue = CreatePen(PS_SOLID, 4, RGB(0,0,255));
            HPEN lgRed = CreatePen(PS_SOLID, 4, RGB(255,0,0));
            SelectObject(drawDC, lgBlue);
            MoveToEx(drawDC, lx, ly+6, NULL); LineTo(drawDC, lx+30, ly+6);
            // 使用Unicode码点确保中文正确显示
            const wchar_t* blueText = L"蓝线：当前内存 (实时)"; // 蓝线：当前内存 (实时)
            const wchar_t* redText = L"红线：历史峰值 (峰值随时间更新)"; // 红线：历史峰值 (峰值随时间更新)
                         TextOutW(drawDC, lx+36, ly, blueText, (int)wcslen(blueText));
             SelectObject(drawDC, lgRed);
             MoveToEx(drawDC, lx, ly+26, NULL); LineTo(drawDC, lx+30, ly+26);
             TextOutW(drawDC, lx+36, ly+20, redText, (int)wcslen(redText));
            DeleteObject(lgBlue); DeleteObject(lgRed);

            // 如果鼠标在图中，显示坐标信息
            if (g_mouseInGraph && g_mouseX >= 0 && count > 0) {
                // 根据鼠标X坐标计算对应的数据点索引
                int relativeX = g_mouseX - graphLeft;
                if (relativeX < 0) relativeX = 0;
                if (relativeX > graphWidth) relativeX = graphWidth;
                int idx = count > 1 ? (relativeX * (count - 1)) / graphWidth : 0;
                if (idx < 0) idx = 0;
                if (idx >= count) idx = count - 1;
                double curVal = (double)g_memHistory[idx];
                double peakVal = (double)g_peakHistory[idx];
                std::wstringstream sss;
                // time label
                std::wstring timeStr = L"";
                if (idx < (int)g_timeHistory.size()) {
                    std::tm bt;
                    localtime_s(&bt, &g_timeHistory[idx]);
                    wchar_t buf[64];
                    wcsftime(buf, 64, L"%H:%M:%S", &bt);
                    timeStr = buf;
                }
                sss << L"Idx:" << idx << L" (" << timeStr << L")  Cur:" << formatBytes((size_t)curVal) << L"  Peak:" << formatBytes((size_t)peakVal);
                int tx = g_mouseX + 12;
                int ty = g_mouseY + 12;
                
                // 先计算文字大小，确保提示框足够大
                                 SIZE textSize;
                 GetTextExtentPoint32W(drawDC, sss.str().c_str(), (int)sss.str().size(), &textSize);
                int boxWidth = textSize.cx + 16; // 额外的内边距
                int boxHeight = textSize.cy + 12;
                
                // 确保提示框不超出窗口边界
                if (tx + boxWidth > rc.right) tx = rc.right - boxWidth - 5;
                if (ty + boxHeight > rc.bottom / 2) ty = g_mouseY - boxHeight - 5;
                if (tx < 5) tx = 5;
                if (ty < 5) ty = 5;
                
                // 背景
                RECT tr = { tx-4, ty-4, tx + boxWidth, ty + boxHeight };
                HBRUSH hBrush = CreateSolidBrush(RGB(255,255,224));
                FillRect(drawDC, &tr, hBrush);
                // 绘制边框
                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(128,128,128));
                HPEN oldPen = (HPEN)SelectObject(drawDC, borderPen);
                Rectangle(drawDC, tr.left, tr.top, tr.right, tr.bottom);
                SelectObject(drawDC, oldPen);
                DeleteObject(borderPen);
                
                                 SetTextColor(drawDC, RGB(20,20,20));
                 RECT textRect = { tx + 4, ty + 2, tx + boxWidth - 4, ty + boxHeight - 2 };
                 DrawTextW(drawDC, sss.str().c_str(), (int)sss.str().size(), &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                DeleteObject(hBrush);
            }

            DeleteObject(hBlue);
            DeleteObject(hRed);
        }
    // 恢复字体和文本颜色
    SetTextColor(drawDC, RGB(0,0,0));
    if (hOldFont) SelectObject(drawDC, hOldFont);
        // blit back
        BitBlt(hdc, 0, 0, rc.right-rc.left, rc.bottom-rc.top, drawDC, 0, 0, SRCCOPY);
        // cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_DRAWITEM: {
        // 自定义绘制控件
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;
        if (lpDIS->CtlID == 2) { // 按钮ID
            RECT rc = lpDIS->rcItem;
            HDC hdc = lpDIS->hDC;
            
            // 根据按钮状态选择颜色
            COLORREF bgColor, textColor;
            if (lpDIS->itemState & ODS_SELECTED) {
                bgColor = RGB(200, 200, 200); // 按下时的颜色
                textColor = RGB(0, 0, 0);
            } else {
                bgColor = RGB(240, 240, 240); // 正常状态颜色
                textColor = RGB(0, 0, 0);
            }
            
            // 绘制背景
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            
            // 绘制边框
            HPEN hPen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, hOldPen);
            DeleteObject(hPen);
            
            // 绘制文字 - 使用更好的文字对齐
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, textColor);
            HFONT oldFont = NULL;
            if (g_hFontSimSun) oldFont = (HFONT)SelectObject(hdc, g_hFontSimSun);
            
            // 计算文字大小并居中绘制 - 使用Unicode码点
            const wchar_t* buttonText = L"申请内存"; // 申请内存
            SIZE textSize;
            GetTextExtentPoint32W(hdc, buttonText, 4, &textSize);
            int textX = rc.left + (rc.right - rc.left - textSize.cx) / 2;
            int textY = rc.top + (rc.bottom - rc.top - textSize.cy) / 2;
            TextOutW(hdc, textX, textY, buttonText, 4);
            
            if (oldFont) SelectObject(hdc, oldFont);
            
            return TRUE;
        }
        else if (lpDIS->CtlID == 3) { // 列表框ID
            RECT rc = lpDIS->rcItem;
            HDC hdc = lpDIS->hDC;
            
            // 检查该项是否已释放
            bool isFreed = false;
            if (lpDIS->itemID < g_allocations.size()) {
                isFreed = g_allocations[lpDIS->itemID].freed;
            }
            
            // 根据选中状态和释放状态选择颜色
            COLORREF bgColor, textColor;
            if (isFreed) {
                // 已释放的项显示为灰色
                if (lpDIS->itemState & ODS_SELECTED) {
                    bgColor = RGB(180, 180, 180); // 选中时的深灰色背景
                    textColor = RGB(100, 100, 100); // 深灰色文字
                } else {
                    bgColor = RGB(245, 245, 245); // 浅灰色背景
                    textColor = RGB(128, 128, 128); // 灰色文字
                }
            } else {
                // 未释放的项正常显示
                if (lpDIS->itemState & ODS_SELECTED) {
                    bgColor = RGB(0, 120, 215); // 选中时的蓝色背景
                    textColor = RGB(255, 255, 255); // 白色文字
                } else {
                    bgColor = RGB(255, 255, 255); // 白色背景
                    textColor = RGB(0, 0, 0); // 黑色文字
                }
            }
            
            // 绘制背景
            HBRUSH hBrush = CreateSolidBrush(bgColor);
            FillRect(hdc, &rc, hBrush);
            DeleteObject(hBrush);
            
            // 获取列表项文字 - 使用Unicode版本的API
            int textLen = (int)SendMessageW(lpDIS->hwndItem, LB_GETTEXTLEN, lpDIS->itemID, 0);
            if (textLen > 0) {
                wchar_t* buffer = new wchar_t[textLen + 1];
                SendMessageW(lpDIS->hwndItem, LB_GETTEXT, lpDIS->itemID, (LPARAM)buffer);
                
                // 绘制文字
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, textColor);
                HFONT oldFont = NULL;
                if (g_hFontSimSun) oldFont = (HFONT)SelectObject(hdc, g_hFontSimSun);
                
                // 使用DrawTextW确保 Unicode 文字正确显示
                RECT textRect = rc;
                textRect.left += 4; // 左边距
                textRect.right -= 4; // 右边距
                DrawTextW(hdc, buffer, textLen, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                
                if (oldFont) SelectObject(hdc, oldFont);
                delete[] buffer;
            }
            
            return TRUE;
        }
        break;
    }
    case WM_SETCURSOR: {
        // 检查是否在列表框上
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        HWND hChild = ChildWindowFromPoint(hwnd, pt);
        if (hChild == hList) {
            // 获取鼠标下的列表项
            POINT listPt = pt;
            ScreenToClient(hList, &listPt);
            int itemIndex = (int)SendMessage(hList, LB_ITEMFROMPOINT, 0, MAKELPARAM(listPt.x, listPt.y));
            if (HIWORD(itemIndex) == 0 && LOWORD(itemIndex) < g_allocations.size()) {
                if (g_allocations[LOWORD(itemIndex)].freed) {
                    // 已释放的项显示禁用光标
                    SetCursor(LoadCursor(NULL, IDC_NO));
                    return TRUE;
                }
            }
        }
        break;
    }
    case WM_DESTROY:
        for (auto& a : g_allocations)
            if (!a.freed) delete[] (char*)a.ptr;
        // 清理进程句柄
        if (g_targetProcessHandle && g_targetProcessHandle != GetCurrentProcess()) {
            CloseHandle(g_targetProcessHandle);
            g_targetProcessHandle = NULL;
        }
        if (g_hFontSimSun) { DeleteObject(g_hFontSimSun); g_hFontSimSun = NULL; }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // 清空调试日志文件（写入到程序同目录）
    std::wstring logPath = getLogFilePath();
    HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        const char* header = "=== Memory Monitor Debug Log Started ===\r\n";
        DWORD written;
        WriteFile(hFile, header, (DWORD)strlen(header), &written, NULL);
        CloseHandle(hFile);
    }
    writeDebugLog(L"Program started");
    
    srand((unsigned)time(NULL));

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MyWinClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(L"MyWinClass", L"内存峰值监视器",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}