#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    if (argc < 3) {
        printf("Usage: pipe_client <pipe_name> <command...>\n");
        return 1;
    }

    std::string pipe_name = "\\\\.\\pipe\\" + std::string(argv[1]);
    std::string cmd;
    for (int i = 2; i < argc; ++i) {
        if (i > 2)
            cmd += ' ';
        cmd += argv[i];
    }

    // Try multiple times with shorter waits to handle timing issues
    const int max_retries = 3;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    
    for (int retry = 0; retry < max_retries; ++retry) {
        if (WaitNamedPipeA(pipe_name.c_str(), 2000)) {
            pipe = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                break; // Success!
            }
        }
        
        if (retry < max_retries - 1) {
            Sleep(500); // Wait 500ms before retry
        }
    }

    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Pipe %s not available after retries (err %lu)\n", pipe_name.c_str(), GetLastError());
        return 1;
    }

    DWORD written = 0;
    if (!WriteFile(pipe, cmd.c_str(), static_cast<DWORD>(cmd.size()), &written, nullptr)) {
        printf("Failed to write to pipe (err %lu)\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }
    
    CloseHandle(pipe);
    printf("Command sent successfully\n");
    return 0;
}
