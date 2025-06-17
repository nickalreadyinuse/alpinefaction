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

    if (!WaitNamedPipeA(pipe_name.c_str(), 5000)) {
        printf("Pipe %s not available (err %lu)\n", pipe_name.c_str(), GetLastError());
        return 1;
    }

    HANDLE pipe = CreateFileA(pipe_name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("Failed to open pipe %s (err %lu)\n", pipe_name.c_str(), GetLastError());
        return 1;
    }

    DWORD written = 0;
    WriteFile(pipe, cmd.c_str(), static_cast<DWORD>(cmd.size()), &written, nullptr);
    CloseHandle(pipe);
    return 0;
}
