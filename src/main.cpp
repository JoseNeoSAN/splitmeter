// SplitMeter - 代理 / 直连分流流量计
// 界面层：分层窗口（每像素透明）+ GDI+ 绘制，折叠/展开两态，托盘常驻
// 许可：MIT

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <gdiplus.h>

#include <cmath>
#include <string>
#include <vector>

#include "core.h"

using namespace Gdiplus;

namespace {

constexpr wchar_t kClassName[] = L"SplitMeterWindow";
constexpr wchar_t kAppName[] = L"SplitMeter";

constexpr UINT WM_APP_UPDATE = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;

constexpr UINT_PTR kTimerSave = 1;
constexpr UINT_PTR kTimerAnim = 2;
constexpr UINT kMenuAutostart = 101;
constexpr UINT kMenuTopmost = 102;
constexpr UINT kMenuReset = 103;
constexpr UINT kMenuToggle = 104;
constexpr UINT kMenuExit = 105;

// 尺寸（DIP，96 DPI 基准）
// 宽度按内容实测定：标签 25 + 间距 + 速率最长 47，两列并排刚好填满 200，不留空洞。
// 尺寸（DIP，96 DPI 基准）
// 按 12px 实测墨迹排定：标签 22 + 间距 8 + 上行 58 + 间距 8 + 下行 58 + 间距 18
// + 总量 46 + 左右边距 24 = 242，刚好排满，不留无意义的空白。
constexpr float kW = 242.f;
constexpr float kHCol = 26.f;
constexpr float kHExp = 62.f;
constexpr float kRCol = 12.f;
constexpr float kRExp = 14.f;
constexpr int kAnimMs = 180;

// 动态布局用的间距常量（列位由实测文字宽度算出，不再写死）
constexpr float kLabelW = 22.f;     // "代理" 的实际字宽
constexpr float kGapText = 8.f;     // 标签→上行、上行→下行
constexpr float kGapBlock = 20.f;   // 折叠态左右两块之间（竖线居中，两侧各 10）
constexpr float kGapTotal = 18.f;   // 下行→总量，比其他两处略大，作为主次分隔
constexpr float kPadX = 12.f;       // 左右内边距
constexpr float kPadY = 7.f;        // 面板上下内边距

// 展开态行位与总量右边界
constexpr float kExpRow1 = 19.f;
constexpr float kExpRow2 = 43.f;
constexpr float kExpRight = 230.f;

// iOS 深色系统色。层次从亮到暗：蓝/白（主）→ 灰（次）→ 深灰（辅助）
const Color kTextPrimary(255, 255, 255, 255);
const Color kTextTotal(255, 110, 110, 115);  // #6E6E73 systemGray4：总量最次要
const Color kProxy(255, 64, 156, 255);       // #409CFF iOS 亮蓝
const Color kDirect(255, 142, 142, 147);     // #8E8E93 systemGray
const Color kHairline(255, 255, 255, 255);
const Color kPanel(255, 28, 28, 30);

struct Stats {
    double pxUp = 0, pxDown = 0, dxUp = 0, dxDown = 0;
    sm::Totals totals{};
    bool connected = false;
};

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HANDLE g_worker = nullptr;
volatile LONG g_running = 1;
volatile LONG g_resetFlag = 0;
CRITICAL_SECTION g_lock;
Stats g_stats;
sm::Persist g_persist;
bool g_topmost = true;
bool g_expanded = false;
UINT g_dpi = 96;
int g_w = 0;
HBITMAP g_dib = nullptr;
void* g_bits = nullptr;
HDC g_memDC = nullptr;
HICON g_icon = nullptr;
NOTIFYICONDATAW g_nid{};

// 动画
float g_animT = 0.f;
float g_animFrom = 0.f;
float g_animTarget = 0.f;
ULONGLONG g_animStartAt = 0;
bool g_animating = false;
bool g_growUp = false;
int g_fixedBottom = 0;

// 拖拽判定
bool g_pressed = false;
bool g_dragging = false;
POINT g_pressPt{};
ULONGLONG g_pressAt = 0;

// 字体缓存
Font* g_fontLabelCN = nullptr;   // 中文标签 11px（折叠与展开共用）
Font* g_fontValueCol = nullptr;  // 速率数值 12px（折叠与展开共用）
Font* g_fontTotal = nullptr;     // 展开态总量 10px

float DP(float v) {
    // g_dpi 若被异常改成 0，会让所有坐标与字号一起归零 → 整块界面空白。
    // 这里做兜底，任何非法 DPI 一律回退 96。
    UINT d = (g_dpi >= 48 && g_dpi <= 480) ? g_dpi : 96;
    return v * d / 96.f;
}
int DPI(float v) {
    UINT d = (g_dpi >= 48 && g_dpi <= 480) ? g_dpi : 96;
    return static_cast<int>(v * d / 96.f + 0.5f);
}

inline Color Fade(const Color& c, float alpha) {
    int a = static_cast<int>(c.GetA() * alpha);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return Color(static_cast<BYTE>(a), c.GetR(), c.GetG(), c.GetB());
}

// ---------------------------------------------------------------- 字体

FontFamily* PickFamily(const wchar_t* name, const wchar_t* fallback) {
    auto* f = new FontFamily(name);
    if (f->GetLastStatus() == Ok) return f;
    delete f;
    f = new FontFamily(fallback);
    if (f->GetLastStatus() == Ok) return f;
    delete f;
    return new FontFamily(L"Microsoft YaHei UI");
}

void RebuildFonts() {
    delete g_fontLabelCN;
    delete g_fontValueCol;
    delete g_fontTotal;
    static FontFamily* cn = PickFamily(L"Microsoft YaHei UI", L"Microsoft YaHei");
    // 注意：不要用 "Segoe UI Variable Text"。它虽被 GDI+ 报告为可用，
    // 但属于可变字体的命名实例，GDI+ 栅格化会失败 —— 表现为文字位置正确却一个像素都不出。
    static FontFamily* num = PickFamily(L"Segoe UI", L"Microsoft YaHei UI");

    // 折叠与展开统一字号：展开后信息变多，但字号不变，靠排布而不是缩字来容纳
    g_fontLabelCN = new Font(cn, DP(11.f), FontStyleRegular, UnitPixel);
    g_fontValueCol = new Font(num, DP(12.f), FontStyleRegular, UnitPixel);
    g_fontTotal = new Font(num, DP(12.f), FontStyleRegular, UnitPixel);
}

// 按展开态最大尺寸准备画布，动画过程中只改提交高度
bool EnsureSurface() {
    if (g_memDC && g_dib && g_bits) return true;
    if (!g_memDC) {
        HDC screen = GetDC(nullptr);
        g_memDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
    }
    if (!g_memDC) return false;
    if (g_dib) {
        DeleteObject(g_dib);
        g_dib = nullptr;
        g_bits = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -DPI(kHExp);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_dib) return false;
    SelectObject(g_memDC, g_dib);
    return true;
}

// 量字符串宽度（返回 DIP，和布局常量同单位）
// ---------------------------------------------------------------- 绘制辅助

// 量字符串宽度。MeasureString 失败时按字符数估算，保证布局不至于崩掉。
float MeasureText(Graphics& g, const std::wstring& s, Font* font) {
    if (s.empty()) return 0.f;
    StringFormat mf;
    RectF layout(0.f, 0.f, 480.f, 48.f);
    RectF ext(0.f, 0.f, 0.f, 0.f);
    if (g.MeasureString(s.c_str(), -1, font, layout, &mf, &ext) == Ok && ext.Width > 0.f) {
        return ext.Width;
    }
    return static_cast<float>(s.size()) * 6.4f;
}

// 量字符串宽度（返回 DIP，和布局常量同单位）
float MeasureDip(Graphics& g, const std::wstring& s, Font* font) {
    float px = MeasureText(g, s, font);
    UINT d = (g_dpi >= 48 && g_dpi <= 480) ? g_dpi : 96;
    return px * 96.f / static_cast<float>(d);
}

// 量真实墨迹宽度（DIP）。
// MeasureString 给出的是排版宽度，含尾部 "s" 之类的右侧留白，直接拿来做居中会让
// 左右间距差出 4~8px。这里用字形路径的实际外框，得到的才是肉眼看得到的宽度。
float MeasureInk(Graphics& g, const std::wstring& s, Font* font) {
    if (s.empty() || !font) return 0.f;

    FontFamily fam;
    if (font->GetFamily(&fam) != Ok) return MeasureDip(g, s, font);

    GraphicsPath path;
    if (path.AddString(s.c_str(), -1, &fam, font->GetStyle(), font->GetSize(),
                       PointF(0.f, 0.f), nullptr) != Ok) {
        return MeasureDip(g, s, font);
    }
    RectF b(0.f, 0.f, 0.f, 0.f);
    if (path.GetBounds(&b) != Ok || b.Width <= 0.f) return MeasureDip(g, s, font);

    UINT d = (g_dpi >= 48 && g_dpi <= 480) ? g_dpi : 96;
    return b.Width * 96.f / static_cast<float>(d);
}

// 右对齐文字：先量宽、再用 PointF 精确定位。
// 两个坑：1) 给 DrawString 传带 FormatFlags 的自定义 StringFormat 会静默不绘；
//        2) 给 MeasureString 传 nullptr 当 StringFormat 时，ext 可能不被回填。
Status TextRight(Graphics& g, const std::wstring& s, Font* font, const Color& color, float right,
                 float centerY, float alpha) {
    if (s.empty()) return Ok;

    SolidBrush brush(Fade(color, alpha));

    // 量宽：失败或结果异常时按字符数估算，保证永远不会画到看不见的地方
    float wpx = 0.f;
    {
        StringFormat mf;
        RectF layout(0.f, 0.f, DP(right) + 64.f, DP(32));
        RectF ext(0.f, 0.f, 0.f, 0.f);
        Status ms = g.MeasureString(s.c_str(), -1, font, layout, &mf, &ext);
        if (ms == Ok && ext.Width > 0.f && ext.Width < DP(right) + 64.f) {
            wpx = ext.Width;
        } else {
            wpx = static_cast<float>(s.size()) * DP(6.4f);
        }
    }

    float lineH = font->GetHeight(&g);
    if (lineH <= 0.f || lineH > DP(60.f)) lineH = DP(15.f);

    float x = DP(right) - wpx;
    if (x < 0.f) x = 0.f;
    float y = DP(centerY) - lineH / 2.f;
    return g.DrawString(s.c_str(), -1, font, PointF(x, y), &brush);
}

Status TextLeft(Graphics& g, const std::wstring& s, Font* font, const Color& color, float x,
                float centerY, float alpha) {
    if (s.empty()) return Ok;

    SolidBrush brush(Fade(color, alpha));
    float lineH = font->GetHeight(&g);
    return g.DrawString(s.c_str(), -1, font, PointF(DP(x), DP(centerY) - lineH / 2.f), &brush);
}

void BuildRoundRect(GraphicsPath& path, float w, float h, float r) {
    if (h < r * 2) r = h / 2;
    path.AddArc(0.f, 0.f, r * 2, r * 2, 180, 90);
    path.AddArc(w - r * 2, 0.f, r * 2, r * 2, 270, 90);
    path.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0, 90);
    path.AddArc(0.f, h - r * 2, r * 2, r * 2, 90, 90);
    path.CloseFigure();
}

// ---------------------------------------------------------------- 渲染

void Render() {
    if (!g_hwnd || !EnsureSurface()) return;

    float t = g_animT;
    float hDip = kHCol + (kHExp - kHCol) * t;
    float rDip = kRCol + (kRExp - kRCol) * t;
    int w = g_w;
    int h = DPI(hDip);

    // 窗口高度跟随动画
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    int x = rc.left;
    int y = g_growUp ? (g_fixedBottom - h) : rc.top;
    if (rc.bottom - rc.top != h || g_growUp) {
        SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 清空当前使用区域
    ZeroMemory(g_bits, static_cast<size_t>(w) * DPI(kHExp) * 4);

    {
        Graphics g(g_memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // 小字号下 AntiAlias 的抗锯齿会把笔画覆盖压到 60% 左右，字看起来发灰。
        // AntiAliasGridFit 会把字形对齐到像素格并加深笔画，正适合这种 10~12px 的界面字。
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetCompositingMode(CompositingModeSourceOver);

        GraphicsPath path;
        BuildRoundRect(path, DP(kW), DP(hDip), DP(rDip));

        // 面板底色（毛玻璃未生效时的保底观感）
        SolidBrush panel(Color(216, 28, 28, 30));
        g.FillPath(&panel, &path);

        // 玻璃边缘高光，iOS 面板靠这道细描边立住轮廓
        Pen edge(Fade(kHairline, 0.09f), DP(0.5f));
        g.DrawPath(&edge, &path);

        Stats s;
        EnterCriticalSection(&g_lock);
        s = g_stats;
        LeaveCriticalSection(&g_lock);

        float aCollapsed = 1.f - t / 0.5f;
        if (aCollapsed < 0) aCollapsed = 0;
        float aExpanded = (t - 0.25f) / 0.75f;
        if (aExpanded < 0) aExpanded = 0;
        if (aExpanded > 1) aExpanded = 1;

        const bool on = s.connected;
        const float kPrimary = 1.0f;

        std::wstring pxRateUp = on ? sm::FormatRate(s.pxUp) : L"—";
        std::wstring pxRateDown = on ? sm::FormatRate(s.pxDown) : L"—";
        std::wstring dxRateUp = on ? sm::FormatRate(s.dxUp) : L"—";
        std::wstring dxRateDown = on ? sm::FormatRate(s.dxDown) : L"—";
        // 折叠态只显示上下行合计
        std::wstring pxRateAll = on ? sm::FormatRate(s.pxUp + s.pxDown) : L"—";
        std::wstring dxRateAll = on ? sm::FormatRate(s.dxUp + s.dxDown) : L"—";

        // ---- 折叠态：内容整体居中，竖线两侧间距恒等
        if (aCollapsed > 0.01f) {
            float lw = MeasureInk(g, pxRateAll, g_fontValueCol);
            float rw = MeasureInk(g, dxRateAll, g_fontValueCol);
            float blockL = kLabelW + kGapText + lw;
            float blockR = kLabelW + kGapText + rw;

            float contentW = blockL + kGapBlock + blockR;
            float x0 = (kW - contentW) * 0.5f;
            if (x0 < kPadX) x0 = kPadX;

            float xRight = x0 + blockL + kGapBlock;
            // +1 补偿数值首字符的左侧字形留白，让竖线两侧看起来等距
            float divX = x0 + blockL + kGapBlock * 0.5f + 1.f;

            TextLeft(g, L"代理", g_fontLabelCN, kProxy, x0, kHCol / 2.f, aCollapsed);
            TextLeft(g, pxRateAll, g_fontValueCol, kTextPrimary, x0 + kLabelW + kGapText,
                     kHCol / 2.f, aCollapsed * kPrimary);

            Pen divider(Fade(kHairline, aCollapsed * 0.09f), DP(0.5f));
            g.DrawLine(&divider, DP(divX), DP(kPadY), DP(divX), DP(kHCol - kPadY));

            TextLeft(g, L"直连", g_fontLabelCN, kDirect, xRight, kHCol / 2.f, aCollapsed);
            TextLeft(g, dxRateAll, g_fontValueCol, kTextPrimary, xRight + kLabelW + kGapText,
                     kHCol / 2.f, aCollapsed * kPrimary);
        }

        // ---- 展开态：与折叠态同字号，列宽取两行中较宽者以保证上下对齐
        if (aExpanded > 0.01f) {
            Region clip(RectF(0, 0, DP(kW), DP(hDip)));
            g.SetClip(&clip);

            std::wstring pUp = L"↑" + pxRateUp;
            std::wstring pDown = L"↓" + pxRateDown;
            std::wstring dUp = L"↑" + dxRateUp;
            std::wstring dDown = L"↓" + dxRateDown;

            float upW = MeasureInk(g, pUp, g_fontValueCol);
            float tmpW = MeasureInk(g, dUp, g_fontValueCol);
            if (tmpW > upW) upW = tmpW;
            float dnW = MeasureInk(g, pDown, g_fontValueCol);
            tmpW = MeasureInk(g, dDown, g_fontValueCol);
            if (tmpW > dnW) dnW = tmpW;

            // 总量
            std::wstring totalP = sm::FormatBytes(s.totals.pxUp + s.totals.pxDown);
            std::wstring totalD = sm::FormatBytes(s.totals.dxUp + s.totals.dxDown);
            float totalW = MeasureInk(g, totalP, g_fontTotal);
            tmpW = MeasureInk(g, totalD, g_fontTotal);
            if (tmpW > totalW) totalW = tmpW;

            // 整行内容居中：左右边距相等，避免固定左对齐导致右侧空出一大块
            float contentW = kLabelW + kGapText + upW + kGapText + dnW + kGapTotal + totalW;
            float x0 = (kW - contentW) * 0.5f;
            if (x0 < kPadX) x0 = kPadX;

            float xLabel = x0;
            float xUp = x0 + kLabelW + kGapText;
            float xDown = xUp + upW + kGapText;
            float xTotal = xDown + dnW + kGapTotal;

            TextLeft(g, L"代理", g_fontLabelCN, kProxy, xLabel, kExpRow1, aExpanded);
            TextLeft(g, pUp, g_fontValueCol, kTextPrimary, xUp, kExpRow1, aExpanded * kPrimary);
            TextLeft(g, pDown, g_fontValueCol, kTextPrimary, xDown, kExpRow1, aExpanded * kPrimary);
            TextLeft(g, totalP, g_fontTotal, kTextTotal, xTotal, kExpRow1, aExpanded);

            // 分隔线跟着内容走，两端与文字对齐
            Pen hair(Fade(kHairline, aExpanded * 0.07f), DP(0.5f));
            g.DrawLine(&hair, DP(xLabel), DP(31), DP(kW - xLabel), DP(31));

            TextLeft(g, L"直连", g_fontLabelCN, kDirect, xLabel, kExpRow2, aExpanded);
            TextLeft(g, dUp, g_fontValueCol, kTextPrimary, xUp, kExpRow2, aExpanded * kPrimary);
            TextLeft(g, dDown, g_fontValueCol, kTextPrimary, xDown, kExpRow2, aExpanded * kPrimary);
            TextLeft(g, totalD, g_fontTotal, kTextTotal, xTotal, kExpRow2, aExpanded);

            g.ResetClip();
        }
    }

    HDC screen = GetDC(nullptr);
    POINT src{0, 0};
    SIZE size{w, h};
    POINT dst{0, 0};
    GetWindowRect(g_hwnd, &rc);
    dst.x = rc.left;
    dst.y = rc.top;

    // 调试开关：设环境变量 SPLITMETER_DUMP=1 后，每帧把渲染缓冲导出成 BMP，
    // 便于在不截屏的情况下离线检查画面。平时不开，零开销。
    {
        static int dumpOn = -1;
        if (dumpOn < 0) {
            wchar_t v[8]{};
            dumpOn = (GetEnvironmentVariableW(L"SPLITMETER_DUMP", v, 8) > 0) ? 1 : 0;
        }
        if (dumpOn == 1) {
            wchar_t tmp2[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tmp2);
            std::wstring bp = std::wstring(tmp2) + L"splitmeter_frame.bmp";
            FILE* fb = _wfopen(bp.c_str(), L"wb");
            if (fb) {
                int H = DPI(kHExp);
                BITMAPFILEHEADER fh{};
                BITMAPINFOHEADER ih{};
                ih.biSize = sizeof(ih);
                ih.biWidth = w;
                ih.biHeight = -H;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biCompression = BI_RGB;
                ih.biSizeImage = static_cast<DWORD>(w) * H * 4;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + ih.biSizeImage;
                fwrite(&fh, sizeof(fh), 1, fb);
                fwrite(&ih, sizeof(ih), 1, fb);
                fwrite(g_bits, 1, ih.biSizeImage, fb);
                fclose(fb);
            }
        }
    }

    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g_hwnd, screen, &dst, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------- 动画

void AnimTick() {
    float p = static_cast<float>(GetTickCount64() - g_animStartAt) / kAnimMs;
    if (p >= 1.f) {
        p = 1.f;
        g_animating = false;
        KillTimer(g_hwnd, kTimerAnim);
    }
    float e = 1.f - std::pow(1.f - p, 3.f);  // ease-out cubic
    g_animT = g_animFrom + (g_animTarget - g_animFrom) * e;
    Render();
}

void SetExpanded(bool expand, bool animate) {
    g_expanded = expand;
    if (!animate) {
        g_animT = expand ? 1.f : 0.f;
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        RECT rc;
        GetWindowRect(g_hwnd, &rc);
        g_growUp = (rc.bottom >= work.bottom - 4);
        g_fixedBottom = rc.bottom;
        Render();
        return;
    }

    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    g_growUp = (rc.bottom >= work.bottom - 4);
    g_fixedBottom = rc.bottom;

    g_animFrom = g_animT;
    g_animTarget = expand ? 1.f : 0.f;
    g_animStartAt = GetTickCount64();
    if (!g_animating) {
        g_animating = true;
        SetTimer(g_hwnd, kTimerAnim, 15, nullptr);
    }
}

void ToggleExpanded() { SetExpanded(!g_expanded, true); }

// ---------------------------------------------------------------- 托盘

void AddTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, kAppName, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

HICON MakeIcon() {
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    GraphicsPath path;
    path.AddArc(2, 2, 28, 28, 180, 90);
    path.AddArc(2, 2, 28, 28, 270, 90);
    path.AddArc(2, 2, 28, 28, 0, 90);
    path.AddArc(2, 2, 28, 28, 90, 90);
    path.CloseFigure();
    SolidBrush bg(Color(255, 32, 32, 36));
    g.FillPath(&bg, &path);

    SolidBrush up(Color(255, 191, 90, 242));
    SolidBrush down(Color(255, 99, 230, 226));
    PointF a[] = {{11, 14}, {16, 8}, {21, 14}};
    g.FillPolygon(&up, a, 3);
    g.FillRectangle(&up, RectF(14.5f, 13, 3, 5));
    PointF b[] = {{11, 18}, {16, 24}, {21, 18}};
    g.FillPolygon(&down, b, 3);
    g.FillRectangle(&down, RectF(14.5f, 14, 3, 5));

    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// ---------------------------------------------------------------- 开机自启

bool IsAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    bool found = RegQueryValueExW(key, kAppName, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf),
                                  &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

void SetAutostart(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_WRITE, &key) != ERROR_SUCCESS) {
        return;
    }
    if (on) {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(quoted.c_str()),
                       static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

// ---------------------------------------------------------------- 工作线程

DWORD WINAPI WorkerProc(LPVOID) {
    sm::Splitter splitter;
    std::wstring pipe;
    int failCount = 0;

    EnterCriticalSection(&g_lock);
    splitter.reset();
    splitter.seed(g_stats.totals);
    LeaveCriticalSection(&g_lock);

    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        if (InterlockedExchange(&g_resetFlag, 0) != 0) {
            EnterCriticalSection(&g_lock);
            splitter.reset();
            LeaveCriticalSection(&g_lock);
        }

        if (pipe.empty()) pipe = sm::FindCorePipe();

        sm::CoreSnapshot snap;
        bool ok = !pipe.empty() && sm::PollCore(pipe, snap);

        if (!ok) {
            if (++failCount >= 2) {
                pipe = sm::FindCorePipe();
                failCount = 0;
            }
        } else {
            failCount = 0;
        }

        if (ok) {
            EnterCriticalSection(&g_lock);
            splitter.feed(snap);
            g_stats.pxUp = splitter.pxUpRate();
            g_stats.pxDown = splitter.pxDownRate();
            g_stats.dxUp = splitter.dxUpRate();
            g_stats.dxDown = splitter.dxDownRate();
            g_stats.totals = splitter.totals();
            g_stats.connected = true;
            LeaveCriticalSection(&g_lock);
        } else {
            EnterCriticalSection(&g_lock);
            g_stats.pxUp = g_stats.pxDown = g_stats.dxUp = g_stats.dxDown = 0;
            g_stats.connected = false;
            LeaveCriticalSection(&g_lock);
        }

        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UPDATE, 0, 0);
        Sleep(500);
    }
    return 0;
}

// ---------------------------------------------------------------- 菜单

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuToggle, g_expanded ? L"收起" : L"展开");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), kMenuAutostart, L"开机自启");
    AppendMenuW(menu, MF_STRING | (g_topmost ? MF_CHECKED : 0), kMenuTopmost, L"窗口置顶");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuReset, L"清零本次统计");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------- 窗口过程

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_UPDATE:
            Render();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_LBUTTONDOWN: {
            g_pressed = true;
            g_dragging = false;
            g_pressPt.x = GET_X_LPARAM(lp);
            g_pressPt.y = GET_Y_LPARAM(lp);
            g_pressAt = GetTickCount64();
            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!g_pressed || g_dragging) return 0;
            int dx = GET_X_LPARAM(lp) - g_pressPt.x;
            int dy = GET_Y_LPARAM(lp) - g_pressPt.y;
            int threshold = DPI(3.f);
            if (dx * dx + dy * dy > threshold * threshold) {
                g_dragging = true;
                g_pressed = false;
                ReleaseCapture();
                RECT work;
                SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                g_growUp = (rc.bottom >= work.bottom - 4);
                g_fixedBottom = rc.bottom;
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_pressed && !g_dragging) {
                ReleaseCapture();
                g_pressed = false;
                ToggleExpanded();
            }
            g_pressed = false;
            return 0;
        }

        case WM_CAPTURECHANGED:
            g_pressed = false;
            return 0;

        case WM_RBUTTONUP:
            ShowMenu();
            return 0;

        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
            return 0;

        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case kMenuToggle:
                    ToggleExpanded();
                    return 0;
                case kMenuAutostart:
                    SetAutostart(!IsAutostart());
                    return 0;
                case kMenuTopmost:
                    g_topmost = !g_topmost;
                    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE);
                    return 0;
                case kMenuReset: {
                    EnterCriticalSection(&g_lock);
                    g_stats.totals = sm::Totals{};
                    LeaveCriticalSection(&g_lock);
                    InterlockedExchange(&g_resetFlag, 1);
                    g_persist.totals = sm::Totals{};
                    sm::SavePersist(g_persist);
                    Render();
                    return 0;
                }
                case kMenuExit:
                    DestroyWindow(hwnd);
                    return 0;
            }
            return 0;
        }

        case WM_TIMER:
            if (wp == kTimerAnim) {
                AnimTick();
            } else if (wp == kTimerSave) {
                RECT rc;
                GetWindowRect(hwnd, &rc);
                g_persist.x = rc.left;
                g_persist.y = rc.top;
                EnterCriticalSection(&g_lock);
                g_persist.totals = g_stats.totals;
                LeaveCriticalSection(&g_lock);
                g_persist.bootId = sm::CurrentBootId();
                sm::SavePersist(g_persist);
            }
            return 0;

        case WM_WINDOWPOSCHANGED: {
            // 拖动后位置变了，保存一下（动画期间由 Render 自行处理）
            if (!g_animating) {
                WINDOWPOS* p = reinterpret_cast<WINDOWPOS*>(lp);
                if (!(p->flags & SWP_NOMOVE)) {
                    g_persist.x = p->x;
                    g_persist.y = p->y;
                }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }

        case WM_DPICHANGED: {
            UINT newDpi = HIWORD(wp);
            if (newDpi < 48 || newDpi > 480) newDpi = LOWORD(wp);
            if (newDpi < 48 || newDpi > 480) return 0;  // 无效值直接忽略，别把界面搞成 0 尺寸
            g_dpi = newDpi;
            g_w = DPI(kW);
            RebuildFonts();
            if (g_memDC) {
                DeleteDC(g_memDC);
                g_memDC = nullptr;
            }
            if (g_dib) {
                DeleteObject(g_dib);
                g_dib = nullptr;
                g_bits = nullptr;
            }
            RECT* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, g_w,
                         DPI(g_expanded ? kHExp : kHCol), SWP_NOZORDER | SWP_NOACTIVATE);
            Render();
            return 0;
        }

        case WM_DESTROY: {
            InterlockedExchange(&g_running, 0);
            if (g_worker) {
                WaitForSingleObject(g_worker, 2000);
                CloseHandle(g_worker);
                g_worker = nullptr;
            }
            RECT rc;
            GetWindowRect(hwnd, &rc);
            g_persist.x = rc.left;
            g_persist.y = g_growUp ? g_fixedBottom - DPI(kHCol) : rc.top;
            EnterCriticalSection(&g_lock);
            g_persist.totals = g_stats.totals;
            LeaveCriticalSection(&g_lock);
            g_persist.bootId = sm::CurrentBootId();
            sm::SavePersist(g_persist);

            RemoveTray();
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"SplitMeter_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_icon = MakeIcon();
    InitializeCriticalSection(&g_lock);

    sm::Persist loaded;
    if (sm::LoadPersist(loaded)) {
        g_persist = loaded;
    }
    uint64_t boot = sm::CurrentBootId();
    bool freshBoot = false;
    if (g_persist.bootId == 0 || boot < g_persist.bootId || boot - g_persist.bootId > 120) {
        g_persist.totals = sm::Totals{};
        freshBoot = true;
    }
    g_persist.bootId = boot;

    if (freshBoot) {
        std::wstring pipe = sm::FindCorePipe();
        sm::CoreSnapshot snap;
        if (!pipe.empty() && sm::PollCore(pipe, snap)) {
            uint64_t coreStart = sm::CoreProcessStartTime();
            if (coreStart == 0 || coreStart + 120 >= boot) {
                uint64_t px = 0, dx = 0;
                for (auto& c : snap.conns) {
                    uint64_t v = c.up + c.down;
                    if (c.proxied) {
                        px += v;
                    } else {
                        dx += v;
                    }
                }
                double share = (px + dx) ? static_cast<double>(px) / static_cast<double>(px + dx) : 1.0;
                g_persist.totals.pxUp = static_cast<uint64_t>(snap.upTotal * share);
                g_persist.totals.pxDown = static_cast<uint64_t>(snap.downTotal * share);
                g_persist.totals.dxUp = snap.upTotal - g_persist.totals.pxUp;
                g_persist.totals.dxDown = snap.downTotal - g_persist.totals.pxDown;
            }
        }
        g_stats.totals = g_persist.totals;
    }

    GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    GdiplusStartup(&gdiToken, &gdiIn, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    g_dpi = GetDpiForSystem();
    if (g_dpi < 48 || g_dpi > 480) g_dpi = 96;
    g_w = DPI(kW);
    RebuildFonts();

    int x = g_persist.x, y = g_persist.y;
    if (x == INT32_MIN || y == INT32_MIN) {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        x = work.right - g_w - 24;
        y = work.bottom - DPI(kHCol) - 24;
    }

    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             kClassName, kAppName, WS_POPUP, x, y, g_w, DPI(kHCol), nullptr,
                             nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    {
        HDC screen = GetDC(nullptr);
        g_memDC = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        EnsureSurface();
    }

    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AddTray();
    g_animT = 0.f;
    Render();
    SetTimer(g_hwnd, kTimerSave, 5000, nullptr);

    g_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete g_fontLabelCN;
    delete g_fontValueCol;
    delete g_fontTotal;
    DeleteCriticalSection(&g_lock);
    GdiplusShutdown(gdiToken);
    return 0;
}
