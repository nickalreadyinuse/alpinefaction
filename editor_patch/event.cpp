#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <memory>
#include <iomanip>
#include <sstream>
#include <string>
#include <common/version/version.h>
#include <common/config/BuildConfig.h>
#include <common/utils/os-utils.h>
#include <xlog/xlog.h>
#include <xlog/ConsoleAppender.h>
#include <xlog/FileAppender.h>
#include <xlog/Win32Appender.h>
#include <patch_common/MemUtils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <crash_handler_stub.h>
#include "../game_patch/rf/os/array.h"
#include "exports.h"
#include "resources.h"
#include "mfc_types.h"
#include "vtypes.h"
#include "event.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE) & __ImageBase)

// Custom event support
constexpr int original_event_count = 89;
constexpr int new_event_count = 26; // must be 1 higher than actual count
constexpr int total_event_count = original_event_count + new_event_count;
std::unique_ptr<const char*[]> extended_event_names; // array to hold original + additional event names

// master list of new events, last one is dummy for counting (ignore)
const char* additional_event_names[new_event_count] = {
    "SetVar",
    "Clone_Entity",
    "Set_Player_World_Collide",
    "Switch_Random",
    "Difficulty_Gate",
    "HUD_Message",
    "Play_Video",
    "Set_Level_Hardness",
    "Sequence",
    "Clear_Queued",
    "Remove_Link",
    "Fixed_Delay",
    "Add_Link",
    "Valid_Gate",
    "Goal_Math",
    "Goal_Gate",
    "Environment_Gate",
    "Inside_Gate",
    "Anchor_Marker",
    "Force_Unhide",
    "Set_Difficulty",
    "Set_Fog_Far_Clip",
    "AF_When_Dead",
    "Gametype_Gate",
    "When_Picked_Up",
    "_dummy"
};

void initialize_event_names()
{
    // allocate space for total event names
    extended_event_names = std::make_unique<const char*[]>(total_event_count + 1);
    extended_event_names[total_event_count] = nullptr; // padding to prevent overrun

    // reference the stock event names array in memory
    const char** original_event_names = reinterpret_cast<const char**>(0x00578B78);

    // read and populate extended_event_names with stock event names
    for (int i = 0; i < original_event_count; ++i) {
        if (original_event_names[i]) {
            extended_event_names[i] = original_event_names[i];
            //xlog::info("Loaded original event name [{}]: {}", i, original_event_names[i]);
        }
        else { // should never be hit, including for safety
            //xlog::warn("Original event name [{}] is null or corrupted", i);
            extended_event_names[i] = nullptr;
        }
    }

    // add new event names to extended_event_names
    for (int i = 0; i < new_event_count; ++i) {
        extended_event_names[original_event_count + i] = additional_event_names[i];
        //xlog::info("Added additional event name [{}]: {}", original_event_count + i, additional_event_names[i]);
    }

    xlog::info("Initialized extended_event_names with {} entries", total_event_count);
}

// verify the event names in extended_event_names LOGGING
void debug_event_names()
{
    for (int i = 0; i < total_event_count; ++i) {
        if (extended_event_names[i]) {
            //xlog::info("Debug: Event name [{}]: {}", i, extended_event_names[i]);
        }
        else {
            xlog::warn("Debug: Event name [{}] is null or corrupted", i);
        }
    }
};

// verify the event names in extended_event_names LOGGING
void verify_event_names()
{
    for (int i = 0; i < total_event_count; ++i) {
        if (extended_event_names[i]) {
            //xlog::info("Event name [{}]: {}", i, extended_event_names[i]);
        }
        else {
            xlog::warn("Event name [{}] is null or corrupted", i);
        }
    }
}

// in CDedLevel__OpenEventPropertiesInternal
CodeInjection event_names_injection{
    0x00407782,
    [](auto& regs) {
        using namespace asm_regs;

        // look up index for selected event in new array
        int index = regs.eax;
        regs.edx = reinterpret_cast<uintptr_t>(extended_event_names[index]);

        regs.eip = 0x00407789;
    }
};

// in CEventDialog__OnInitDialog
CodeInjection OnInitDialog_redirect_event_names{
    0x004617EA, [](auto& regs) {
        // update reference to old event_names array with new extended_event_names
        regs.edi = reinterpret_cast<uintptr_t>(extended_event_names.get());

        for (int i = 0; i < total_event_count; ++i) {
            //xlog::info("Attempting to access extended_event_names[{}]: {}", i, extended_event_names[i]);
        }

        regs.eip = 0x004617EF;
    }
};

// in get_event_type_from_class_name
CodeInjection get_event_type_redirect_event_names{
    0x004516A9,
    [](auto& regs) {
        using namespace asm_regs;
        // update reference to old event_names array with new extended_event_names
        regs.esi = reinterpret_cast<uintptr_t>(extended_event_names.get());

        for (int i = 0; i < total_event_count; ++i) {
            //xlog::info("Also Attempting to access extended_event_names[{}]: {}", i, extended_event_names[i]);
        }
      
        regs.eip = 0x004516AE;
    }
};

// convert wide string to narrow string
std::string wide_to_narrow(const std::wstring& wide_str) {
    std::ostringstream oss;
    for (wchar_t wc : wide_str) {
        oss << static_cast<char>(wc);
    }
    return oss.str();
}

// convert narrow string to wide string
std::wstring narrow_to_wide(const std::string& narrow_str) {
    std::wostringstream woss;
    for (char c : narrow_str) {
        woss << static_cast<wchar_t>(c);
    }
    return woss.str();
}

// temporary storage of currently active alpine event
DedEvent* currentDedEvent = nullptr;

std::map<int, FieldConfig> eventFieldConfigs = {
    {89, {
        "Alpine Event Properties - Set_Var", 
        {FIELD_STR1, FIELD_STR2},
        {
            {FIELD_STR1, "Variable to change:"},
            {FIELD_STR2, "New value:"}
        }
    }},

};

void CreateDynamicControls(HWND hwndDlg, const FieldConfig& config)
{
    HDC hdc = GetDC(hwndDlg); // Get device context to measure font size
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    int controlHeight = tm.tmHeight + 8; // Base control height on font metrics
    int labelWidth = 120;
    int fieldWidth = 200;
    int xLabel = 10;
    int xField = xLabel + labelWidth + 10;
    int yOffset = 10;

    HFONT hDefaultFont = (HFONT)SendMessage(hwndDlg, WM_GETFONT, 0, 0);

    // Include mandatory fields
    std::vector<FieldType> allFields = {FIELD_SCRIPT_NAME, FIELD_DELAY};
    allFields.insert(allFields.end(), config.fieldsToShow.begin(), config.fieldsToShow.end());

    for (FieldType field : allFields) {
        std::string label = config.fieldLabels.count(field) ? config.fieldLabels.at(field) : "";

        if (field == FIELD_SCRIPT_NAME)
            label = "Script Name:";
        if (field == FIELD_DELAY)
            label = "Delay:";

        // Create label
        HWND hLabel = CreateWindowW(L"STATIC", narrow_to_wide(label).c_str(), WS_VISIBLE | WS_CHILD, xLabel, yOffset,
                                    labelWidth, controlHeight, hwndDlg, nullptr, nullptr, nullptr);
        SendMessage(hLabel, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

        // Create the control
        HWND hField;
        if (field == FIELD_BOOL1 || field == FIELD_BOOL2) {
            hField = CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, xField, yOffset, 20,
                                   controlHeight, hwndDlg, reinterpret_cast<HMENU>(field), nullptr, nullptr);
        }
        else {
            hField =
                CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, xField, yOffset,
                              fieldWidth, controlHeight, hwndDlg, reinterpret_cast<HMENU>(field), nullptr, nullptr);
        }

        SendMessage(hField, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

        yOffset += controlHeight + 10;
    }

    // Add OK and Cancel buttons
    int buttonWidth = 80;
    int buttonHeight = 25;
    int xOK = xLabel;
    int xCancel = xField + fieldWidth - buttonWidth;

    HWND hOKButton = CreateWindowW(L"BUTTON", L"OK", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, xOK, yOffset, buttonWidth,
                                   buttonHeight, hwndDlg, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
    SendMessage(hOKButton, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

    HWND hCancelButton =
        CreateWindowW(L"BUTTON", L"Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, xCancel, yOffset, buttonWidth,
                      buttonHeight, hwndDlg, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
    SendMessage(hCancelButton, WM_SETFONT, (WPARAM)hDefaultFont, TRUE);

    yOffset += buttonHeight + 40;
    ReleaseDC(hwndDlg, hdc);

    // Resize the dialog
    int dialogWidth = xField + fieldWidth + 20;
    int dialogHeight = yOffset;
    SetWindowPos(hwndDlg, nullptr, 0, 0, dialogWidth, dialogHeight, SWP_NOMOVE | SWP_NOZORDER);
}




INT_PTR CALLBACK DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    char buffer[256];

    switch (uMsg) {
    case WM_INITDIALOG: {
        if (currentDedEvent) {
            FieldConfig config = eventFieldConfigs[currentDedEvent->event_type];

            // Set dialog title
            SetWindowTextW(hwndDlg, narrow_to_wide(config.windowTitle).c_str());

            // Create dynamic controls
            CreateDynamicControls(hwndDlg, config);

            // Populate mandatory fields
            SetDlgItemTextA(hwndDlg, FIELD_SCRIPT_NAME, currentDedEvent->script_name.c_str());
            SetDlgItemTextA(hwndDlg, FIELD_DELAY, std::to_string(currentDedEvent->delay).c_str());

            // Populate specific fields
            for (FieldType field : config.fieldsToShow) {
                switch (field) {
                case FIELD_INT1:
                    SetDlgItemInt(hwndDlg, FIELD_INT1, currentDedEvent->int1, TRUE);
                    break;
                case FIELD_INT2:
                    SetDlgItemInt(hwndDlg, FIELD_INT2, currentDedEvent->int2, TRUE);
                    break;
                case FIELD_FLOAT1:
                    SetDlgItemTextA(hwndDlg, FIELD_FLOAT1, std::to_string(currentDedEvent->float1).c_str());
                    break;
                case FIELD_FLOAT2:
                    SetDlgItemTextA(hwndDlg, FIELD_FLOAT2, std::to_string(currentDedEvent->float2).c_str());
                    break;
                case FIELD_BOOL1:
                    CheckDlgButton(hwndDlg, FIELD_BOOL1, currentDedEvent->bool1 ? BST_CHECKED : BST_UNCHECKED);
                    break;
                case FIELD_BOOL2:
                    CheckDlgButton(hwndDlg, FIELD_BOOL2, currentDedEvent->bool2 ? BST_CHECKED : BST_UNCHECKED);
                    break;
                case FIELD_STR1:
                    SetDlgItemTextA(hwndDlg, FIELD_STR1, currentDedEvent->str1.c_str());
                    break;
                case FIELD_STR2:
                    SetDlgItemTextA(hwndDlg, FIELD_STR2, currentDedEvent->str2.c_str());
                    break;
                default:
                    break;
                    //xlog::warn("Unhandled field type in WM_INITDIALOG: {}", field);
                }
            }
        }
        return TRUE;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == IDOK && currentDedEvent) {
            FieldConfig config = eventFieldConfigs[currentDedEvent->event_type];

            xlog::info("Saving dialog for event_type: {}", currentDedEvent->event_type);

            // Save mandatory fields
            GetDlgItemTextA(hwndDlg, FIELD_SCRIPT_NAME, buffer, sizeof(buffer));
            currentDedEvent->script_name.assign_0(buffer);
            xlog::info("Saved Script Name: {}", currentDedEvent->script_name.c_str());

            GetDlgItemTextA(hwndDlg, FIELD_DELAY, buffer, sizeof(buffer));
            currentDedEvent->delay = std::stof(buffer);
            xlog::info("Saved Delay: {}", currentDedEvent->delay);

            // Save specific fields
            for (FieldType field : config.fieldsToShow) {
                //xlog::info("Saving field: {}", field);
                switch (field) {
                case FIELD_INT1:
                    currentDedEvent->int1 = GetDlgItemInt(hwndDlg, FIELD_INT1, nullptr, TRUE);
                    xlog::info("Saved Int1: {}", currentDedEvent->int1);
                    break;
                case FIELD_INT2:
                    currentDedEvent->int2 = GetDlgItemInt(hwndDlg, FIELD_INT2, nullptr, TRUE);
                    xlog::info("Saved Int2: {}", currentDedEvent->int2);
                    break;
                case FIELD_FLOAT1:
                    GetDlgItemTextA(hwndDlg, FIELD_FLOAT1, buffer, sizeof(buffer));
                    currentDedEvent->float1 = std::stof(buffer);
                    xlog::info("Saved Float1: {}", currentDedEvent->float1);
                    break;
                case FIELD_FLOAT2:
                    GetDlgItemTextA(hwndDlg, FIELD_FLOAT2, buffer, sizeof(buffer));
                    currentDedEvent->float2 = std::stof(buffer);
                    xlog::info("Saved Float2: {}", currentDedEvent->float2);
                    break;
                case FIELD_BOOL1:
                    currentDedEvent->bool1 = IsDlgButtonChecked(hwndDlg, FIELD_BOOL1) == BST_CHECKED;
                    xlog::info("Saved Bool1: {}", currentDedEvent->bool1);
                    break;
                case FIELD_BOOL2:
                    currentDedEvent->bool2 = IsDlgButtonChecked(hwndDlg, FIELD_BOOL2) == BST_CHECKED;
                    xlog::info("Saved Bool2: {}", currentDedEvent->bool2);
                    break;
                case FIELD_STR1:
                    GetDlgItemTextA(hwndDlg, FIELD_STR1, buffer, sizeof(buffer));
                    currentDedEvent->str1.assign_0(buffer);
                    xlog::info("Saved Str1: {}", currentDedEvent->str1.c_str());
                    break;
                case FIELD_STR2:
                    GetDlgItemTextA(hwndDlg, FIELD_STR2, buffer, sizeof(buffer));
                    currentDedEvent->str2.assign_0(buffer);
                    xlog::info("Saved Str2: {}", currentDedEvent->str2.c_str());
                    break;
                default:
                    break;
                    //xlog::warn("Unhandled field in WM_COMMAND: {}", field);
                }
            }


            EndDialog(hwndDlg, IDOK);
            return TRUE;
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hwndDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    default:
        return FALSE;
    }
    return FALSE;
}



bool ShowAlpineEventDialog(HWND parent, DedEvent* dedEvent)
{
    currentDedEvent = dedEvent;

    // Use DialogBoxParam for modal behavior
    int result =
        DialogBoxParam(HINST_THISCOMPONENT, MAKEINTRESOURCE(IDD_ALPINE_EVENT_PROPERTIES), nullptr, DialogProc, 0);

    if (result == -1) {
        DWORD error = GetLastError();
        xlog::error("DialogBox failed with error code: {}", error);
    }
    currentDedEvent = nullptr; // Reset after dialog ends

    return (result == IDOK);
}



void OpenAlpineEventProperties(DedEvent* dedEvent) {
    if (ShowAlpineEventDialog( nullptr, dedEvent)) {
        xlog::info("Dialog saved successfully!");
    }
    else {
        xlog::warn("Dialog canceled.");
    }
}

// set template, in CDedLevel__OpenEventProperties (needs cleanup)
CodeInjection open_event_properties_patch{
    0x00408D6D, [](auto& regs) {
        //using namespace asm_regs;
        //int event_type = static_cast<int>(regs.ecx);
        DedEvent* event = regs.ebx;
        xlog::warn("opening properties on event {}, type {}", event->class_name.c_str(), event->event_type);

        if (event->event_type > 88) { // only redirect alpine events
            OpenAlpineEventProperties(event);
            regs.eip = 0x00408F34;
        }
    }
};

void ApplyEventsPatches()
{
    // Support custom event integration
    initialize_event_names(); // populate extended array with stock + AF events
    // debug_event_names(); // debug logging

    // assign extended events array
    OnInitDialog_redirect_event_names.install();   // replace reference to event_names with new extended array
    get_event_type_redirect_event_names.install(); // replace reference to event_names with new extended array
    event_names_injection.install(); // when opening event properties, use new extended array for event look up

    // handle event properties windows for AF events
    open_event_properties_patch.install(); // redirect AF events to new properties dialog

    // set new end address for event array loops that use new extended array
    AsmWriter(0x004617FC) // OnInitDialog
        .cmp(asm_regs::edi, reinterpret_cast<uintptr_t>(&extended_event_names[total_event_count - 1]));
    AsmWriter(0x004516C2) // get_event_type_from_class_name
        .cmp(asm_regs::esi, reinterpret_cast<uintptr_t>(&extended_event_names[total_event_count - 1]));

    // verify_event_names(); // debug logging

    // Make Set_Liquid_Depth show in events panel, Original code omits that event by name
    AsmWriter(0x004440B4).push("_dummy");
}
