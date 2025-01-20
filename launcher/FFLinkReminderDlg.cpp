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

    // Set the default state of the checkbox to unchecked
    CheckDlgButton(IDC_DONT_SHOW_AGAIN, BST_UNCHECKED);
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
    }

    return CDialog::OnCommand(wparam, lparam);
}

void FFLinkReminderDlg::OnLearnMoreClicked()
{
    ShellExecuteW(nullptr, L"open", L"https://www.redfactionwiki.com/wiki/Link_Alpine_Faction_to_a_FactionFiles_Account", nullptr, nullptr, SW_SHOW);
    m_dont_show_again = (IsDlgButtonChecked(IDC_DONT_SHOW_AGAIN) == BST_CHECKED);
    EndDialog(IDNO);
}

void FFLinkReminderDlg::OnContinueClicked()
{
    m_dont_show_again = (IsDlgButtonChecked(IDC_DONT_SHOW_AGAIN) == BST_CHECKED);
    EndDialog(IDNO);
}
