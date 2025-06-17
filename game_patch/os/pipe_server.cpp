#include "pipe_server.h"
#include "../rf/os/os.h"
#include "../rf/os/console.h"
#include "../rf/multi.h"
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
    xlog::debug("Pipe thread started for pipe '{}'", full_name);
    printf("[PIPE] Thread started for pipe '%s'\n", full_name.c_str());
    fflush(stdout);
    
    while (g_running.load()) {
        xlog::debug("Creating named pipe '{}'", full_name);
        printf("[PIPE] Creating named pipe '%s'\n", full_name.c_str());
        fflush(stdout);
        
        HANDLE pipe = CreateNamedPipeA(full_name.c_str(), PIPE_ACCESS_INBOUND,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 0, 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            xlog::error("CreateNamedPipe failed (err {})", error);
            printf("[PIPE] CreateNamedPipe FAILED (err %lu)\n", error);
            fflush(stdout);
            return;
        }
        
        xlog::debug("Named pipe '{}' created successfully, waiting for client connection", full_name);
        printf("[PIPE] Named pipe '%s' created successfully, waiting for client connection\n", full_name.c_str());
        fflush(stdout);
        
        // Wait for a client to connect
        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected) {
            DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) {
                xlog::error("ConnectNamedPipe failed (err {})", error);
                printf("[PIPE] ConnectNamedPipe failed (err %lu)\n", error);
                fflush(stdout);
                CloseHandle(pipe);
                continue;
            }
        }
        
        xlog::debug("Client connected to pipe '{}'", full_name);
        printf("[PIPE] Client connected to pipe '%s'\n", full_name.c_str());
        fflush(stdout);
        
        char buf[512];
        DWORD bytes_read = 0;
        while (g_running.load() && ReadFile(pipe, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read) {
            buf[bytes_read] = '\0';
            std::string cmd{buf};
            while (!cmd.empty() && (cmd.back() == '\r' || cmd.back() == '\n'))
                cmd.pop_back();
            if (!cmd.empty()) {
                xlog::debug("Received command via pipe: '{}'", cmd);
                printf("[PIPE] Received command: '%s'\n", cmd.c_str());
                fflush(stdout);
                rf::console::do_command(cmd.c_str());
            }
        }
        
        xlog::debug("Client disconnected from pipe '{}'", full_name);
        printf("[PIPE] Client disconnected from pipe '%s'\n", full_name.c_str());
        fflush(stdout);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    
    xlog::debug("Pipe thread terminated for pipe '{}'", full_name);
    printf("[PIPE] Thread terminated for pipe '%s'\n", full_name.c_str());
    fflush(stdout);
}

void named_pipe_server_pre_init()
{
    xlog::debug("Named pipe server pre-init called");
    printf("[PIPE] Pre-init called\n");
    fflush(stdout);
    get_pipe_cmd_line_param();
}

void named_pipe_server_init()
{
    xlog::debug("Named pipe server init called");
    printf("[PIPE] Init called\n");
    fflush(stdout);
    
    if (!get_pipe_cmd_line_param().found()) {
        xlog::debug("Pipe command line parameter not found - pipe server disabled");
        printf("[PIPE] Command line parameter NOT found - pipe server disabled\n");
        fflush(stdout);
        return;
    }

    printf("[PIPE] Command line parameter found!\n");
    fflush(stdout);

    if (!rf::is_dedicated_server) {
        xlog::warn("Named pipe server started on non-dedicated server");
        printf("[PIPE] WARNING: Started on non-dedicated server\n");
        fflush(stdout);
    }

    g_pipe_name = get_pipe_cmd_line_param().get_arg();
    xlog::debug("Pipe name from command line: '{}'", g_pipe_name);
    printf("[PIPE] Pipe name from command line: '%s'\n", g_pipe_name.c_str());
    fflush(stdout);
    
    if (g_pipe_name.empty()) {
        xlog::debug("Pipe name is empty - pipe server disabled");
        printf("[PIPE] Pipe name is EMPTY - pipe server disabled\n");
        fflush(stdout);
        return;
    }

    xlog::debug("Starting pipe server thread for pipe '{}'", g_pipe_name);
    printf("[PIPE] Starting pipe server thread for pipe '%s'\n", g_pipe_name.c_str());
    fflush(stdout);
    
    g_running = true;
    g_thread = std::thread(pipe_thread_proc, g_pipe_name);
    xlog::info("Named pipe server started on {}", g_pipe_name);
    printf("[PIPE] Named pipe server started on '%s'\n", g_pipe_name.c_str());
    fflush(stdout);
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
