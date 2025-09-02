#include <windows.h>
#include <psapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <cstring>  // for memset

#include <windowsx.h>
#include <iomanip>

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

HWND hEdit, hButton, hList;
int g_mouseX = -1, g_mouseY = -1;
bool g_mouseInGraph = false;
HFONT g_hFontSimSun = NULL;
// 添加防抖动变量
static DWORD g_lastMouseMove = 0;
static const DWORD MOUSE_THROTTLE_MS = 50; // 50ms防抖动

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
    ss << L"内存块 " << index << L" | " << size / 1024 << L" KB";
    SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;

        hEdit = CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER,
            10, height / 2 + 10, 100, 25, hwnd, (HMENU)1, NULL, NULL);

        hButton = CreateWindowW(L"BUTTON", L"申请内存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            120, height / 2 + 10, 120, 30, hwnd, (HMENU)2, NULL, NULL);

        hList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
            10, height / 2 + 50, width - 20, height / 2 - 60, hwnd, (HMENU)3, NULL, NULL);

        SetTimer(hwnd, 1, 2000, NULL);
        // 预分配容器大小，减少内存重新分配
        g_memHistory.reserve(250);
        g_peakHistory.reserve(250);
        g_timeHistory.reserve(250);
        // 创建宋体并应用到控件
        g_hFontSimSun = CreateFontW(14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FF_DONTCARE,L"宋体");
        if (g_hFontSimSun) {
            SendMessage(hEdit, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hButton, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
            SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSimSun, TRUE);
        }
                 // 设置列表框行高，确保文字完整显示
         SendMessageW(hList, LB_SETITEMHEIGHT, 0, 20);
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
    case WM_COMMAND: {
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
                 TextOutW(drawDC, 14, y - 8, ss.str().c_str(), (int)ss.str().size());
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
    if (g_hFontSimSun) { DeleteObject(g_hFontSimSun); g_hFontSimSun = NULL; }
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    srand((unsigned)time(NULL));

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MyWinClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(L"MyWinClass", L"内存峰值监视器",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInst, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}