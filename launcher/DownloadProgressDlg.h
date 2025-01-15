#pragma once

#include <string>
#include <wxx_dialog.h>
#include <wxx_wincore.h>

#define WM_UPDATE_PROGRESS (WM_USER + 1)
#define WM_DOWNLOAD_COMPLETE (WM_USER + 2)

class DownloadProgressDlg : public CDialog
{
public:
    DownloadProgressDlg(int file_id, const std::string& file_name, size_t file_size_kb);
    void UpdateProgress(unsigned bytes_received);

protected:
    BOOL OnInitDialog() override;
    LRESULT OnUpdateProgress(WPARAM wparam, LPARAM lparam);
    LRESULT OnDownloadComplete(WPARAM wparam, LPARAM lparam);
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam);

private:
    int m_file_id;
    std::string m_file_name;
    size_t m_file_size_kb; // File size in KB
};
