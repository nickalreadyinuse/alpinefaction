#include <wxx_wincore.h>
#include "OptionsInterfaceDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsInterfaceDlg::OptionsInterfaceDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_INTERFACE), m_conf(conf)
{
}

BOOL OptionsInterfaceDlg::OnInitDialog()
{
    // Attach controls
    AttachItem(IDC_LANG_COMBO, m_lang_combo);

    // Populate combo boxes with static content
    m_lang_combo.AddString("Auto");
    m_lang_combo.AddString("English");
    m_lang_combo.AddString("German");
    m_lang_combo.AddString("French");

    InitToolTip();

    CheckDlgButton(IDC_KEEP_LAUNCHER_OPEN_CHECK, m_conf.keep_launcher_open ? BST_CHECKED : BST_UNCHECKED);
    m_lang_combo.SetCurSel(m_conf.language + 1);

    return TRUE;
}

void OptionsInterfaceDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_KEEP_LAUNCHER_OPEN_CHECK), "Keep launcher window open after game or editor launch");
}

void OptionsInterfaceDlg::OnSave()
{
    m_conf.keep_launcher_open = (IsDlgButtonChecked(IDC_KEEP_LAUNCHER_OPEN_CHECK) == BST_CHECKED);
    m_conf.language = m_lang_combo.GetCurSel() - 1;
}
