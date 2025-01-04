#include <memory>
#include "ImageButton.h"
#include "resource.h"

ImageButton::ImageButton() :
    hBmpNormal(nullptr), hBmpHover(nullptr), hBmpPressed(nullptr), isHovering(false), isPressed(false)
{}

ImageButton::~ImageButton()
{
    DeleteObject(hBmpNormal);
    DeleteObject(hBmpHover);
    DeleteObject(hBmpPressed);
}

void ImageButton::LoadImages(int normalID, int hoverID, int pressedID)
{
    hBmpNormal =
        (HBITMAP)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(normalID), IMAGE_BITMAP, 0, 0, LR_LOADTRANSPARENT);
    if (!hBmpNormal)
        MessageBox(NULL, "Failed to load normal bitmap!", 0);

    hBmpHover =
        (HBITMAP)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(hoverID), IMAGE_BITMAP, 0, 0, LR_LOADTRANSPARENT);
    if (!hBmpHover)
        MessageBox(NULL, "Failed to load hover bitmap!", 0);

    hBmpPressed =
        (HBITMAP)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(pressedID), IMAGE_BITMAP, 0, 0, LR_LOADTRANSPARENT);
    if (!hBmpPressed)
        MessageBox(NULL, "Failed to load pressed bitmap!", 0);
}


void ImageButton::PreRegisterClass(WNDCLASS& wc)
{
    wc.style = CS_HREDRAW | CS_VREDRAW;
}

LRESULT ImageButton::WndProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!isHovering) {
            isHovering = true;
            Invalidate();

            // track mouse location
            auto tme = std::make_unique<TRACKMOUSEEVENT>();
            tme->cbSize = sizeof(TRACKMOUSEEVENT);
            tme->dwFlags = TME_LEAVE;
            tme->hwndTrack = GetHwnd();

            // handle unhover
            if (!TrackMouseEvent(tme.get())) {
            }
        }
        break;

    case WM_MOUSELEAVE:
        isHovering = false;
        Invalidate();
        break;

    case WM_LBUTTONDOWN:
        isPressed = true;
        Invalidate();
        break;

    case WM_LBUTTONUP:
        isPressed = false;
        Invalidate();
        SendMessage(GetParent(), WM_COMMAND, GetDlgCtrlID(), 0); // Simulate button click
        break;

    default:
        return WndProcDefault(msg, wparam, lparam);
    }

    return 0;
}


void ImageButton::DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct)
{
    HDC hdc = lpDrawItemStruct->hDC;
    HDC hdcMem = CreateCompatibleDC(hdc);

    HBITMAP hBmpToDraw = isPressed ? hBmpPressed : (isHovering ? hBmpHover : hBmpNormal);
    SelectObject(hdcMem, hBmpToDraw);

    RECT rc = lpDrawItemStruct->rcItem;
    BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, hdcMem, 0, 0, SRCCOPY);

    DeleteDC(hdcMem);
}
