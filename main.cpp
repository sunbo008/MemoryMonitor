#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <ctime>

#include <windowsx.h>
#include <iomanip>

struct Allocation {
    void* ptr;
    size_t size;
    bool freed;
};

std::vector<Allocation> g_allocations;
std::vector<size_t> g_memHistory, g_peakHistory;
std::vector<time_t> g_timeHistory;

HWND hEdit, hButton, hList;
int g_mouseX = -1, g_mouseY = -1;
bool g_mouseInGraph = false;
HFONT g_hFontSimSun = NULL;

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
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.WorkingSetSize;
    }
    return 0;
}
size_t getPeakRSS() {
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return counters.PeakWorkingSetSize;
    }
    return 0;
}

void addListItem(HWND list, size_t size, int index) {
    std::wstringstream ss;
    ss << L"Block " << index << L" | " << size / 1024 << L" KB";
    SendMessage(list, LB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        hEdit = CreateWindow(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10, height / 2 + 10, 100, 25, hwnd, (HMENU)1, NULL, NULL);

        hButton = CreateWindow(L"BUTTON", L"申请内存", WS_CHILD | WS_VISIBLE,
            120, height / 2 + 10, 100, 25, hwnd, (HMENU)2, NULL, NULL);

        hList = CreateWindow(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
            10, height / 2 + 50, width - 20, height / 2 - 60, hwnd, (HMENU)3, NULL, NULL);

        SetTimer(hwnd, 1, 1000, NULL);
        // 创建宋体并应用到控件
        g_hFontSimSun = CreateFontW(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FF_DONTCARE,L"宋体");
        if (g_hFontSimSun) {
            SendMessage(hEdit, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hButton, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
        }
        break;
    }
    case WM_TIMER: {
        g_memHistory.push_back(getCurrentRSS());
        g_peakHistory.push_back(getPeakRSS());
        g_timeHistory.push_back(time(NULL));
        if (g_memHistory.size() > 200) {
            g_memHistory.erase(g_memHistory.begin());
            g_peakHistory.erase(g_peakHistory.begin());
            g_timeHistory.erase(g_timeHistory.begin());
        }
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    }
    case WM_COMMAND: {
        if (LOWORD(wParam) == 2) { // 申请按钮
            wchar_t buf[64];
            GetWindowText(hEdit, buf, 64);
            int size = _wtoi(buf);
            if (size == 0) size = (rand() % 50 + 1) * 1024 * 1024; // 随机1-50MB
            char* p = new char[size];
            for (int i = 0; i < size; i++) p[i] = 'a' + (rand() % 26);

            Allocation a{ p, (size_t)size, false };
            g_allocations.push_back(a);
            addListItem(hList, size, (int)g_allocations.size() - 1);
        }
        else if (LOWORD(wParam) == 3 && HIWORD(wParam) == LBN_DBLCLK) { // 双击列表释放
            int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
            if (sel != LB_ERR && sel < (int)g_allocations.size()) {
                if (!g_allocations[sel].freed) {
                    delete[] (char*)g_allocations[sel].ptr;
                    g_allocations[sel].freed = true;

                    wchar_t buf[256];
                    SendMessage(hList, LB_GETTEXT, sel, (LPARAM)buf);
                    std::wstring s(buf);
                    s += L" [已释放]";
                    SendMessage(hList, LB_DELETESTRING, sel, 0);
                    SendMessage(hList, LB_INSERTSTRING, sel, (LPARAM)s.c_str());
                }
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int mid = (rc.bottom - rc.top) / 2;
        int mx = LOWORD(lParam);
        int my = HIWORD(lParam);
        if (my < mid) {
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
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: {
        g_mouseInGraph = false;
        g_mouseX = g_mouseY = -1;
        InvalidateRect(hwnd, NULL, FALSE);
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

        // 绘制网格
        HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(50,50,50));
        HPEN lightGrid = CreatePen(PS_SOLID, 1, RGB(40,40,40));
        SelectObject(drawDC, gridPen);
        int graphTop = 0;
        int graphBottom = mid - 10;
        int graphLeft = 0;
        int graphRight = rc.right;
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

            // 缩放
            size_t maxVal = 1;
            for (size_t v : g_peakHistory) if (v > maxVal) maxVal = v;

            // 绘制 Y 轴刻度和标签 (自动单位)
            // 使用已经选中的宋体字体
            for (int t = 0; t <= 4; t++) {
                double frac = t / 4.0;
                int y = mid - (int)(frac * (mid - 20));
                MoveToEx(drawDC, 6, y, NULL);
                LineTo(drawDC, 12, y);
                // Label
                size_t val = (size_t)(maxVal * frac);
                std::wstringstream ss;
                ss << formatBytes(val);
                // 以浅色字体显示
                SetTextColor(drawDC, RGB(180,180,180));
                SetBkMode(drawDC, TRANSPARENT);
                TextOut(drawDC, 14, y - 8, ss.str().c_str(), (int)ss.str().size());
            }

            // 蓝线：当前内存
            // 先构造点数组并填充下面的区域
            std::vector<POINT> pts;
            pts.reserve(count + 2);
            for (int i = 0; i < count; ++i) {
                int x = i * 3;
                int y = mid - (int)((double)g_memHistory[i] / maxVal * (mid - 20));
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
            MoveToEx(drawDC, 0, mid - (int)((double)g_peakHistory[0] / maxVal * (mid - 20)), NULL);
            for (int i = 1; i < count; i++) {
                LineTo(drawDC, i * 3, mid - (int)((double)g_peakHistory[i] / maxVal * (mid - 20)));
            }

            // 绘制图例
            int lx = rc.right - 240;
            int ly = 10;
            HPEN lgBlue = CreatePen(PS_SOLID, 4, RGB(0,0,255));
            HPEN lgRed = CreatePen(PS_SOLID, 4, RGB(255,0,0));
            SelectObject(drawDC, lgBlue);
            MoveToEx(drawDC, lx, ly+6, NULL); LineTo(drawDC, lx+30, ly+6);
            TextOut(drawDC, lx+36, ly, L"蓝线：当前内存 (实时)", 12);
            SelectObject(drawDC, lgRed);
            MoveToEx(drawDC, lx, ly+26, NULL); LineTo(drawDC, lx+30, ly+26);
            TextOut(drawDC, lx+36, ly+20, L"红线：历史峰值 (峰值随时间更新)", 18);
            DeleteObject(lgBlue); DeleteObject(lgRed);

            // 如果鼠标在图中，显示坐标信息
            if (g_mouseInGraph && g_mouseX >= 0) {
                int idx = g_mouseX / 3;
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
                // 背景
                RECT tr = { tx-4, ty-4, tx + 360, ty + 26 };
                HBRUSH hBrush = CreateSolidBrush(RGB(255,255,224));
                FillRect(drawDC, &tr, hBrush);
                SetTextColor(drawDC, RGB(20,20,20));
                DrawText(drawDC, sss.str().c_str(), (int)sss.str().size(), &tr, DT_LEFT | DT_NOPREFIX);
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
    case WM_DESTROY:
        for (auto& a : g_allocations)
            if (!a.freed) delete[] (char*)a.ptr;
    if (g_hFontSimSun) { DeleteObject(g_hFontSimSun); g_hFontSimSun = NULL; }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    srand((unsigned)time(NULL));

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MyWinClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    HWND hwnd = CreateWindow(L"MyWinClass", L"内存峰值监视器",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}