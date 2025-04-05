#include <wxx_wincore.h>
#include "OptionsDlg.h"
#include "LauncherApp.h"
#include <xlog/xlog.h>
#include <wxx_dialog.h>
#include <wxx_commondlg.h>

OptionsDlg::OptionsDlg() :
    CDialog(IDD_OPTIONS),
    m_display_dlg(m_conf),
    m_misc_dlg(m_conf),
    m_multiplayer_dlg(m_conf)
{
}

BOOL OptionsDlg::OnInitDialog()
{
    try {
        m_conf.load();
    }
    catch (std::exception &e) {
        MessageBoxA(e.what(), nullptr, MB_ICONERROR | MB_OK);
    }

    SetDlgItemTextA(IDC_EXE_PATH_EDIT, m_conf.game_executable_path->c_str());

    // Init nested dialogs
    InitNestedDialog(m_display_dlg, IDC_DISPLAY_OPTIONS_BOX);
    InitNestedDialog(m_misc_dlg, IDC_MISC_OPTIONS_BOX);
    InitNestedDialog(m_multiplayer_dlg, IDC_MULTIPLAYER_OPTIONS_BOX);

    InitToolTip();

    return TRUE;
}

void OptionsDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_EXE_PATH_EDIT), "Path to RF.exe file in Red Faction directory");
}

void OptionsDlg::OnOK()
{
    OnBnClickedOk();
    CDialog::OnOK();
}

BOOL OptionsDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);

    UINT id = LOWORD(wparam);
    switch (id) {
    case IDC_EXE_BROWSE:
        OnBnClickedExeBrowse();
        return TRUE;
    }

    return FALSE;
}

void OptionsDlg::OnBnClickedOk()
{
    // fix for strange bug that would blank exe path field if changing force port or maxfps
    if (!GetDlgItemTextA(IDC_EXE_PATH_EDIT).GetString().empty()) {
        m_conf.game_executable_path = GetDlgItemTextA(IDC_EXE_PATH_EDIT).GetString();
    }
    
    // Nested dialogs
    m_display_dlg.OnSave();
    m_multiplayer_dlg.OnSave();
    m_misc_dlg.OnSave();

    try {
        m_conf.save();
    }
    catch (std::exception &e) {
        MessageBoxA(e.what(), nullptr, MB_ICONERROR | MB_OK);
    }
}

void OptionsDlg::OnBnClickedExeBrowse()
{
    LPCTSTR filter = "Executable Files (*.exe)|*.exe|All Files (*.*)|*.*||";
    auto exe_path = GetDlgItemTextA(IDC_EXE_PATH_EDIT);
    CFileDialog dlg(TRUE, ".exe", exe_path, OFN_HIDEREADONLY, filter);
    dlg.SetTitle("Select game executable (RF.exe)");

    if (dlg.DoModal(*this) == IDOK)
        SetDlgItemText(IDC_EXE_PATH_EDIT, dlg.GetPathName());
}

void OptionsDlg::InitNestedDialog(CDialog& dlg, int placeholder_id)
{
    dlg.DoModeless(GetHwnd());
    CWnd placeholder = GetDlgItem(placeholder_id);
    RECT rc = placeholder.GetClientRect();
    RECT border = { 6, 12, 6, 6 };
    MapDialogRect(border);
    rc.left += border.left;
    rc.top += border.top;
    placeholder.MapWindowPoints(GetHwnd(), rc);
    dlg.SetWindowPos(placeholder, rc.left, rc.top, -1, -1, SWP_NOSIZE | SWP_NOACTIVATE);
}
