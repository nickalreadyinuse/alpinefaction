#include <wxx_wincore.h>
#include "OptionsInputDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsInputDlg::OptionsInputDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_INPUT), m_conf(conf)
{
}

BOOL OptionsInputDlg::OnInitDialog()
{
    InitToolTip();



    return TRUE;
}

void OptionsInputDlg::InitToolTip()
{
    m_tool_tip.Create(*this);

}

void OptionsInputDlg::OnSave()
{

}
