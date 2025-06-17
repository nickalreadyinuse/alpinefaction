#include "pipe_server.h"
#include "../rf/os/os.h"
#include "../rf/os/console.h"
#include <windows.h>
#include <xlog/xlog.h>
#include <string>
#include <thread>
#include <atomic>

static rf::CmdLineParam& get_pipe_cmd_line_param()
{
    static rf::CmdLineParam pipe_param{"-pipe", "", true};
    return pipe_param;
}

static std::atomic_bool g_running{false};
static std::thread g_thread;
static std::string g_pipe_name;

static void pipe_thread_proc(std::string pipe_name)
{
    std::string full_name = "\\\\.\\pipe\\" + pipe_name;
    while (g_running.load()) {
        HANDLE pipe = CreateNamedPipeA(full_name.c_str(), PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       1, 0, 0, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            xlog::error("CreateNamedPipe failed (err {})", GetLastError());
            return;
        }
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        char buf[512];
        DWORD bytes_read = 0;
        while (g_running.load() && ReadFile(pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read) {
            buf[bytes_read] = '\0';
            std::string cmd{buf};
            while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n'))
                cmd.pop_back();
            if (!cmd.empty())
                rf::console::do_command(cmd.c_str());
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void named_pipe_server_pre_init()
{
    get_pipe_cmd_line_param();
}

void named_pipe_server_init()
{
    if (!get_pipe_cmd_line_param().found())
        return;

    if (!rf::is_dedicated_server) {
        xlog::warn("Named pipe server started on non-dedicated server");
    }

    g_pipe_name = get_pipe_cmd_line_param().get_arg();
    if (g_pipe_name.empty())
        return;

    g_running = true;
    g_thread = std::thread(pipe_thread_proc, g_pipe_name);
    xlog::info("Named pipe server started on {}", g_pipe_name);
}

void named_pipe_server_shutdown()
{
    if (!g_running)
        return;
    g_running = false;

    if (!g_pipe_name.empty()) {
        std::string full_name = "\\\\.\\pipe\\" + g_pipe_name;
        HANDLE pipe = CreateFileA(full_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            CloseHandle(pipe);
    }

    if (g_thread.joinable())
        g_thread.join();
    g_pipe_name.clear();
    xlog::info("Named pipe server stopped");
}
