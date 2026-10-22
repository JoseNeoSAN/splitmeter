// 测量各字符串在真实字体下的像素宽度，用来精确设计紧凑布局。
#include <windows.h>
#include <gdiplus.h>

#include <cstdio>

using namespace Gdiplus;

static void measure(Graphics& g, Font* f, const wchar_t* s) {
    RectF ext;
    g.MeasureString(s, -1, f, PointF(0, 0), &ext);
    printf("  %-20ls %6.2f x %5.2f\n", s, ext.Width, ext.Height);
}

int main() {
    GdiplusStartupInput si;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &si, nullptr);

    HDC hdc = GetDC(nullptr);
    {
        Graphics g(hdc);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);

        FontFamily cn(L"Microsoft YaHei UI");
        FontFamily num(L"Segoe UI");

        printf("=== 标签 微软雅黑 %s ===\n", "10.5px");
        Font f1(&cn, 10.5f, FontStyleRegular, UnitPixel);
        measure(g, &f1, L"代理");
        measure(g, &f1, L"直连");

        printf("=== 数值 Segoe UI 11px ===\n");
        Font f2(&num, 11.f, FontStyleRegular, UnitPixel);
        measure(g, &f2, L"0");
        measure(g, &f2, L"999 B/s");
        measure(g, &f2, L"1.19 K/s");
        measure(g, &f2, L"383 K/s");
        measure(g, &f2, L"12.3 M/s");
        measure(g, &f2, L"1.23 G/s");

        printf("=== 展开列 Segoe UI 10.5px ===\n");
        Font f3(&num, 10.5f, FontStyleRegular, UnitPixel);
        measure(g, &f3, L"\u2191 1.02 M/s");
        measure(g, &f3, L"\u2193 2.31 M/s");
        measure(g, &f3, L"\u2191 999 B/s");
        measure(g, &f3, L"\u2193 888 B/s");

        printf("=== 总量 Segoe UI 9.5px ===\n");
        Font f4(&num, 9.5f, FontStyleRegular, UnitPixel);
        measure(g, &f4, L"892 MB");
        measure(g, &f4, L"4.79 GB");
        measure(g, &f4, L"12.34 GB");

        printf("=== 总量 Segoe UI 9px ===\n");
        Font f5(&num, 9.f, FontStyleRegular, UnitPixel);
        measure(g, &f5, L"892 MB");
        measure(g, &f5, L"4.79 GB");

        // 行高
        printf("\n行高: 10.5px=%.2f  11px=%.2f  9.5px=%.2f\n",
               f1.GetHeight(&g), f2.GetHeight(&g), f4.GetHeight(&g));
    }
    ReleaseDC(nullptr, hdc);

    GdiplusShutdown(tok);
    return 0;
}
