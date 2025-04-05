#pragma once

#include <common/config/GameConfig.h>
#include <wxx_wincore.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>
#include "OptionsDisplayDlg.h"
#include "OptionsMiscDlg.h"
#include "OptionsMultiplayerDlg.h"

class OptionsDlg : public CDialog
{
public:
	OptionsDlg();
    OptionsDlg(const OptionsDlg&) = delete;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;

private:
    void InitToolTip();
    void OnBnClickedOk();
    void OnBnClickedExeBrowse();
    void InitNestedDialog(CDialog& dlg, int placeholder_id);

    CToolTip m_tool_tip;
    GameConfig m_conf;
    OptionsDisplayDlg m_display_dlg;
    OptionsMiscDlg m_misc_dlg;
    OptionsMultiplayerDlg m_multiplayer_dlg;
};
