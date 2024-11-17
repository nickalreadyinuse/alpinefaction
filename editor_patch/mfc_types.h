#pragma once

#include <windows.h>
#include <patch_common/MemUtils.h>

struct CCmdTarget_mbrs
{
    // Placeholder struct for baseclass members
    //char placeholder[0x18];
};

struct CCmdTarget_vtbl
{
    // Placeholder struct for virtual table
    //char placeholder[0x50];
};

struct CWnd_mbrs: CCmdTarget_mbrs
{
    //CCmdTarget_mbrs baseclass_0;
    HWND m_hWnd;
    HWND m_hWndOwner;
    unsigned int m_nFlags;
    int(__stdcall* m_pfnSuper)(HWND, unsigned int, unsigned int, int);
    int m_nModalResult;
    void* m_pDropTarget;
    struct COleControlContainer* m_pCtrlCont;
    struct COleControlSite* m_pCtrlSite;
};

struct CWnd_vtbl : CCmdTarget_vtbl
{
    //CCmdTarget_vtbl baseclass_0; // Base class virtual table
    void* PreSubclassWindow;     // Offset 0x50
    void* Create;                // Offset 0x54
    void* DestroyWindow;         // Offset 0x58
    void* PreCreateWindow;       // Offset 0x5C
    void* CalcWindowRect;        // Offset 0x60
    void* OnToolHitTest;         // Offset 0x64
    void* GetScrollBarCtrl;      // Offset 0x68
    void* WinHelp;               // Offset 0x6C
    void* ContinueModal;         // Offset 0x70
    void* EndModalLoop;          // Offset 0x74
    void* OnCommand;             // Offset 0x78
    void* OnNotify;              // Offset 0x7C
    void* GetSuperWndProcAddr;   // Offset 0x80
    void* DoDataExchange;        // Offset 0x84
    void* BeginModalState;       // Offset 0x88
    void* EndModalState;         // Offset 0x8C
    void* PreTranslateMessage;   // Offset 0x90
    void* OnAmbientProperty;     // Offset 0x94
    void* WindowProc;            // Offset 0x98
    void* OnWndMsg;              // Offset 0x9C
    void* DefWindowProc;         // Offset 0xA0
    void* PostNcDestroy;         // Offset 0xA4
    void* OnChildNotify;         // Offset 0xA8
    void* CheckAutoCenter;       // Offset 0xAC
    void* IsFrameWnd;            // Offset 0xB0
    void* SetOccDialogInfo;      // Offset 0xB4
};

struct CWnd
{
    CWnd_vtbl* _vft;
    CWnd_mbrs _d;
};


struct CComboBox : public CWnd
{
    int AddString(const char* lpszString)
    {
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
    char* m_pchData;

    CString() : m_pchData(nullptr) {}

    CString(const char* s)
    {
        assign(s);
    }

    ~CString()
    {
        delete[] m_pchData;
    }

    void assign(const char* s)
    {
        delete[] m_pchData;
        if (s) {
            size_t len = strlen(s) + 1;
            m_pchData = new char[len];
            strcpy(m_pchData, s);
        }
        else {
            m_pchData = nullptr;
        }
    }

    bool empty() const
    {
        return m_pchData == nullptr || m_pchData[0] == '\0';
    }

    operator const char*() const
    {
        return m_pchData ? m_pchData : "";
    }

    const char* c_str() const
    {
        return m_pchData ? m_pchData : "";
    }

    bool operator==(const char* s) const
    {
        return std::strcmp(c_str(), s) == 0;
    }

    bool operator!=(const char* s) const
    {
        return !(*this == s);
    }

    void Format(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);

        // Determine the required length for the formatted string
        size_t size = vsnprintf(nullptr, 0, fmt, args) + 1;

        va_end(args);
        va_start(args, fmt);

        // Allocate memory and format the string
        delete[] m_pchData;
        m_pchData = new char[size];
        vsnprintf(m_pchData, size, fmt, args);

        va_end(args);
    }
};




inline HWND WndToHandle(CWnd* wnd)
{
    return struct_field_ref<HWND>(wnd, 4 + 0x18);
}

// Note object IDs in editor are different from game
enum class DedObjectType : int
{
    DED_CLUTTER = 0x0,
    DED_ENTITY = 0x1,
    DED_ITEM = 0x2,
    DED_RESPAWN_POINT = 0x3,
    DED_TRIGGER = 0x4,
    DED_AMBIENT_SOUND = 0x5,
    DED_LIGHT = 0x7,
    DED_GEO_REGION = 0x8,
    DED_NAV_POINT = 0x9,
    DED_EVENT = 0xA,
    DED_CUTSCENE_CAMERA = 0xB,
    DED_CUTSCENE_PATH_NODE = 0xC,
    DED_PARTICLE_EMITTER = 0xD,
    DED_GAS_REGION = 0xE,
    DED_ROOM_EFFECT = 0xF,
    DED_EAX_EFFECT = 0x10,
    DED_CLIMBING_REGION = 0x11,
    DED_DECAL = 0x12,
    DED_BOLT_EMITTER = 0x13,
    DED_TARGET = 0x14,
    DED_KEYFRAME = 0x15,
    DED_PUSH_REGION = 0x16
};

struct Vector3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vector3() = default;
    Vector3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
};

struct Matrix3
{
    float data[3][3] = {0};

    Matrix3() = default;
};

template<typename T>
struct VArray
{
    int size;     // Number of elements currently stored
    int capacity; // Total capacity of the array (max elements it can hold before resizing)
    T* data_ptr;  // Pointer to the actual data

    // Constructor
    VArray() : size(0), capacity(0), data_ptr(nullptr) {}

    // Destructor to free memory if allocated
    ~VArray()
    {
        delete[] data_ptr;
    }

    // Method to add a new element, handling resizing if necessary
    void add(const T& element)
    {
        if (size >= capacity) {
            resize();
        }
        data_ptr[size++] = element;
    }

    // Access operator to retrieve elements
    T& operator[](size_t index)
    {
        if (index >= static_cast<size_t>(size))
            throw std::out_of_range("Index out of range");
        return data_ptr[index];
    }

    const T& operator[](size_t index) const
    {
        if (index >= static_cast<size_t>(size))
            throw std::out_of_range("Index out of range");
        return data_ptr[index];
    }

    // Get current size
    int get_size() const
    {
        return size;
    }

    // Get current capacity
    int get_capacity() const
    {
        return capacity;
    }

private:
    // Resize the internal array, doubling the capacity
    void resize()
    {
        int new_capacity = (capacity == 0) ? 1 : capacity * 2;
        T* new_data = new T[new_capacity];

        // Copy old data to the new array
        for (int i = 0; i < size; ++i) {
            new_data[i] = data_ptr[i];
        }

        // Free old data and update pointers
        delete[] data_ptr;
        data_ptr = new_data;
        capacity = new_capacity;
    }
};

struct VString
{
    int max_len; // Maximum length of the string
    char* buf;   // Pointer to the character buffer

    VString() : max_len(0), buf(nullptr) {}

    VString(const char* str)
    {
        if (str) {
            max_len = static_cast<int>(strlen(str));
            buf = new char[max_len + 1];
            strcpy(buf, str);
        }
        else {
            max_len = 0;
            buf = nullptr;
        }
    }

    // Destructor to clean up dynamically allocated memory
    ~VString()
    {
        delete[] buf;
    }

    // Access the string as a C-style string
    const char* c_str() const
    {
        return buf ? buf : "";
    }

    // Assignment operator for managing string memory
    VString& operator=(const char* str)
    {
        delete[] buf; // Free existing buffer
        if (str) {
            max_len = static_cast<int>(strlen(str));
            buf = new char[max_len + 1];
            strcpy(buf, str);
        }
        else {
            max_len = 0;
            buf = nullptr;
        }
        return *this;
    }

    bool empty() const
    {
        return buf == nullptr || buf[0] == '\0';
    }

    // Prevent copy semantics to avoid double deletion
    VString(const VString&) = delete;
    VString& operator=(const VString&) = delete;

    // Allow move semantics
    VString(VString&& other) noexcept : max_len(other.max_len), buf(other.buf)
    {
        other.max_len = 0;
        other.buf = nullptr;
    }

    VString& operator=(VString&& other) noexcept
    {
        if (this != &other) {
            delete[] buf;
            max_len = other.max_len;
            buf = other.buf;
            other.max_len = 0;
            other.buf = nullptr;
        }
        return *this;
    }
};


struct Color
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

struct DedObject
{
    void* vtbl;
    VString field_4;
    void* vmesh;
    int field_10;
    Vector3 pos;
    Matrix3 orient;
    VString script_name;
    int uid;
    VString class_name;
    bool hidden_in_editor;
    char padding1[3];
    DedObjectType type;
    Vector3 field_60;
    Vector3 field_6C;
    char field_78;
    char padding2[3];
    VArray<int> links;
    VArray<std::string> field_88;
};

struct DedEvent : DedObject
{
    //DedObject baseclass_0;
    int event_type;
    float delay;
    Color color;
    int int1;
    int int2;
    float float1;
    float float2;
    bool bool1;
    bool bool2;
    char padding[2];
    VString str1;
    VString str2;
};

struct CDialog_mbrs : CWnd_mbrs
{
    //CWnd_mbrs baseclass_0;
    UINT m_nIDHelp;
    LPCTSTR m_lpszTemplateName;
    HGLOBAL m_hDialogTemplate;
    LPCDLGTEMPLATE m_lpDialogTemplate;
    void* m_lpDialogInit;
    void* m_pParentWnd;
    HWND m_hWndTop;
    int m_pOccDialogInfo;
};

struct CDialog_vtbl : CWnd_vtbl
{
    //CWnd_vtbl baseclass_0; // Base class virtual table entries
    void* DoModal;         // Offset 0xB8 // overridden in CDialog
    void* OnInitDialog;    // Offset 0xBC
    void* OnSetFont;       // Offset 0xC0
    void* OnOK;            // Offset 0xC4 // overridden in CDialog
    void* OnCancel;        // Offset 0xC8
    void* PreInitDialog;   // Offset 0xCC
};

struct CDialog
{
    CDialog_vtbl* _vft;
    CDialog_mbrs _d;

    int __thiscall DoModal()
    {
        return AddrCaller{0x0052F425}.this_call<int>(this);
    }

    void OnOK()
    {
        AddrCaller{0x0052F712}.this_call<void>(this);
    }

    signed int UpdateData(BOOL bSaveAndValidate)
    {
        return AddrCaller{0x00532575}.this_call<signed int>(this, bSaveAndValidate);
    }
};


struct CEdit
{
    // placeholder struct, can be defined later if needed
};

struct CStatic
{
    // placeholder struct, can be defined later if needed
};

struct CButton
{
    // placeholder struct, can be defined later if needed
};

struct CEventDialog : CDialog
{
    //CDialog baseclass_0;
    CEdit field_5C;
    CString field_98;
    CString field_9C; // script name field
    CStatic field_A0;
    CWnd field_DC;
    CString field_118; // delay field
    CStatic field_11C;
    CComboBox field_158;
    CString field_194; // class name field
    CButton field_198;
    CStatic field_1D4;
    CComboBox field_210;
    CString field_24C;
    CEdit field_250;
    CStatic field_28C;
    CString field_2C8;
    CEdit field_2CC;
    CStatic field_308;
    CString field_344;
    int field_348;
    CStatic field_34C;
    CWnd field_388;
    CString field_3C4;
    CStatic field_3C8;
    CWnd field_404;
    CString field_440;
    CStatic field_444;
    CEdit field_480;
    CString field_4BC;
    CStatic field_4C0;
    CWnd field_4FC;
    CString field_538;
    CStatic field_53C;
    CWnd field_578;
    CString field_5B4;
    CStatic field_5B8;
    CEdit field_5F4;
    CString field_630;
    int field_634;
    CStatic field_638;
    CComboBox field_674;
    CString field_6B0;
    CButton field_6B4;
    int field_6F0;
    int field_6F4;
    CStatic field_6F8;
    CEdit field_734;
    CString field_770;
    CStatic field_774;
    CEdit field_7B0;
    CString field_7EC;
    int field_7F0;
    CStatic field_7F4;
    CEdit field_830;
    CString field_86C;
    int field_870;
    CStatic field_874;
    CEdit field_8B0;
    CString field_8EC;
    CStatic field_8F0;
    CComboBox field_92C;
    int field_968;
    CStatic field_96C;
    CEdit field_9A8;
    CString field_9E4;
    CStatic field_9E8;
    CEdit field_A24;
    CEdit field_A60;
    int field_A9C;
    CStatic field_AA0;
    CEdit field_ADC;
    CString field_B18;
    CStatic field_B1C;
    CComboBox field_B58;
    int field_B94;
    CStatic field_B98;
    CWnd field_BD4;
    CString field_C10;
    CStatic field_C14;
    CWnd field_C50;
    CString field_C8C;
    CStatic field_C90;
    CComboBox field_CCC;
    CString field_D08;
    CStatic field_D0C;
    CWnd field_D48;
    CString field_D84;
    CStatic field_D88;
    CEdit field_DC4;
    CString field_E00;
    int field_E04;
    CStatic field_E08;
    CEdit field_E44;
    CString field_E80;
    CStatic field_E84;
    CEdit field_EC0;
    CString field_EFC;
    int field_F00;
    CStatic field_F04;
    CComboBox field_F40;
    CString field_F7C;
    CStatic field_F80;
    CComboBox field_FBC;
    CString field_FF8;
    CStatic field_FFC;
    CWnd field_1038;
    CString field_1074;
    int field_1078;
    CStatic field_107C;
    CEdit field_10B8;
    CString field_10F4;
    CStatic field_10F8;
    CComboBox field_1134;
    CString field_1170;
    int field_1174;
    int field_1178;
    CStatic field_117C;
    CComboBox field_11B8;
    int field_11F4;
    CStatic field_11F8;
    CComboBox field_1234;
    int field_1270;
    CStatic field_1274;
    CComboBox field_12B0;
    int field_12EC;
    CStatic field_12F0;
    CComboBox field_132C;
    int field_1368;
    CStatic field_136C;
    CComboBox field_13A8;
    int field_13E4;
    CStatic field_13E8;
    CEdit field_1424;
    CString field_1460;
    CStatic field_1464;
    CEdit field_14A0;
    int field_14DC;
    int field_14E0;
    int field_14E4;
    int field_14E8;
    int field_14EC;
    int field_14F0;
    int field_14F4;
    CStatic field_14F8[2];
    CEdit field_1570[2];
    CString field_15E8[2];
    CStatic field_15F0[2];
    CEdit field_1668[2];
    CString field_16E0[2];
    CStatic field_16E8;
    char field_1724[3264];
    CString field_23E4;
    char field_23E8;
    char field_23E9;
    int field_23EC;
    int field_23F0; // uid
    int field_23F4; // something with links, maybe link count?
    int field_23F8;
};

// console is still broken
//static auto& console_print_cmd_list = addr_as_ref<int()>(0x004D4FF0);
//static auto& console_open = addr_as_ref<char()>(0x004D66A0);
//static auto& console_visible = *reinterpret_cast<bool*>(0x0171C214);
//static auto& console_is_visible = addr_as_ref<bool()>(0x004D66C0);
//static auto& console_update = addr_as_ref<void(bool)>(0x004D58C0);
//static auto& console_init = addr_as_ref<void(char)>(0x004D66F0);
