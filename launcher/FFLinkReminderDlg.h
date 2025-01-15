#pragma once

#include <wxx_dialog.h>
#include <wxx_wincore.h>

class FFLinkReminderDlg : public CDialog
{
public:
    FFLinkReminderDlg();

    bool ShouldShowAgain() const
    {
        return !m_dont_show_again;
    }

protected:
    BOOL OnInitDialog() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;
    void OnLearnMoreClicked();
    void OnContinueClicked();

private:
    bool m_dont_show_again = false;
};
