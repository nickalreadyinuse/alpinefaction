#pragma once

#include <windows.h>
#include <patch_common/MemUtils.h>

struct CWnd
{
    struct
    {
        HWND m_hWnd;
    } _d;
};

// CComboBox class extending CWnd
struct CComboBox : public CWnd
{
    // Method to add a string to the combo box, mimicking the original function's behavior
    int AddString(const char* lpszString)
    {
        // Send a CB_ADDSTRING message to the combo box to add the string
        return static_cast<int>(SendMessageA(this->_d.m_hWnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lpszString)));
    }
};

struct CDataExchange
{
	BOOL m_bSaveAndValidate;
	CWnd* m_pDlgWnd;
	HWND m_hWndLastControl;
	BOOL m_bEditLastControl;
};

struct CString
{
    LPTSTR m_pchData;

    void operator=(const char* s)
    {
        AddrCaller{0x0052FBDC}.this_call(this, s);
    }

    operator const char*() const
    {
        return m_pchData;
    }

    bool operator==(const char* s) const
    {
        return std::strcmp(m_pchData, s) == 0;
    }
};

inline HWND WndToHandle(CWnd* wnd)
{
    return struct_field_ref<HWND>(wnd, 4 + 0x18);
}

//static auto& console_print_cmd_list = addr_as_ref<int()>(0x004D4FF0);
//static auto& console_open = addr_as_ref<char()>(0x004D66A0);
//static auto& console_visible = *reinterpret_cast<bool*>(0x0171C214);
//static auto& console_is_visible = addr_as_ref<bool()>(0x004D66C0);
//static auto& console_update = addr_as_ref<void(bool)>(0x004D58C0);
//static auto& console_init = addr_as_ref<void(char)>(0x004D66F0);
