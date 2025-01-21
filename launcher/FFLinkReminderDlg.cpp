#include "FFLinkReminderDlg.h"
#include "resource.h"
#include <shellapi.h>
#include <wxx_dialog.h>
#include <wxx_wincore.h>
#include <xlog/xlog.h>

FFLinkReminderDlg::FFLinkReminderDlg() : CDialog(IDD_FFLINK_REMINDER) {}

BOOL FFLinkReminderDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    CFont m_largeFont;

    m_largeFont.CreatePointFont(16, "MS Shell Dlg");

    // Apply the font to the static text control
    GetDlgItem(IDC_WELCOME_TEXT).SetFont(m_largeFont);

    return TRUE;
}

BOOL FFLinkReminderDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    switch (LOWORD(wparam)) {
    case IDC_LEARN_MORE_BUTTON:
        OnLearnMoreClicked();
        return TRUE;
    case IDC_CONTINUE_BUTTON:
        OnContinueClicked();
        return TRUE;
    case IDC_ACCOUNT_LINK_BUTTON:
        OnAccountLinkClicked();
        return TRUE;
    case IDC_DISCORD_BUTTON:
        OnDiscordClicked();
        return TRUE;
    }

    return CDialog::OnCommand(wparam, lparam);
}

void FFLinkReminderDlg::OnLearnMoreClicked()
{
    ShellExecuteW(nullptr, L"open", L"https://www.factionfiles.com", nullptr, nullptr, SW_SHOW);
}

void FFLinkReminderDlg::OnContinueClicked()
{
    m_dont_show_again = true;
    EndDialog(IDNO);
}

void FFLinkReminderDlg::OnAccountLinkClicked()
{
    ShellExecuteW(nullptr, L"open", L"https://www.redfactionwiki.com/wiki/Link_Alpine_Faction_to_a_FactionFiles_Account", nullptr, nullptr, SW_SHOW);
}

void FFLinkReminderDlg::OnDiscordClicked()
{
    ShellExecuteW(nullptr, L"open", L"https://discord.gg/factionfiles", nullptr, nullptr, SW_SHOW);
}
