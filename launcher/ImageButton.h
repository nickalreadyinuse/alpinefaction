#pragma once

#include <wxx_stdcontrols.h>
#include <wxx_wincore.h>

class ImageButton : public CButton
{
public:
    ImageButton();
    virtual ~ImageButton();

    void LoadImages(int normalID, int hoverID, int pressedID);
    virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);

protected:
    virtual void PreRegisterClass(WNDCLASS& wc);
    virtual LRESULT WndProc(UINT msg, WPARAM wparam, LPARAM lparam);
    

private:
    HBITMAP hBmpNormal;
    HBITMAP hBmpHover;
    HBITMAP hBmpPressed;
    bool isHovering;
    bool isPressed;
};
