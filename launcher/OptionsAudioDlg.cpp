#include <wxx_wincore.h>
#include "OptionsAudioDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsAudioDlg::OptionsAudioDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_AUDIO), m_conf(conf)
{
}

BOOL OptionsAudioDlg::OnInitDialog()
{
    InitToolTip();

    CheckDlgButton(IDC_EAX_SOUND_CHECK, m_conf.eax_sound ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

void OptionsAudioDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_EAX_SOUND_CHECK), "Enable/disable 3D sound and EAX extension if supported");
}

void OptionsAudioDlg::OnSave()
{
    m_conf.eax_sound = (IsDlgButtonChecked(IDC_EAX_SOUND_CHECK) == BST_CHECKED);
}
