#include <windows.h>
#include <atomic>
#include <vector>
#include <xlog/xlog.h>
#include "raw_input.h"

// Raw mouse input is received on a dedicated thread with a message-only window.
// The game's message pump only drains a few messages per frame, so WM_INPUT from a
// high polling rate mouse cannot go through the main window without flooding its queue.
static HANDLE ri_thread_handle = nullptr;
static DWORD ri_thread_id = 0;
static HWND ri_msg_wnd = nullptr;
static HANDLE ri_init_event = nullptr;
static std::atomic<bool> ri_running = false;
static std::atomic<bool> ri_focused = true;
static std::atomic<int> ri_event_count = 0; // total WM_INPUT events received

static SRWLOCK ri_lock = SRWLOCK_INIT;
static int ri_accum_dx = 0;
static int ri_accum_dy = 0;
static std::atomic<bool> ri_absolute_seen = false;
static char ri_absolute_device[256] = {}; // written before ri_absolute_seen is set, read after it is observed

static void log_raw_mouse_devices()
{
    UINT num_devices = 0;
    GetRawInputDeviceList(nullptr, &num_devices, sizeof(RAWINPUTDEVICELIST));
    if (num_devices == 0) {
        xlog::info("Raw input: no HID devices found");
        return;
    }

    std::vector<RAWINPUTDEVICELIST> devices(num_devices);
    GetRawInputDeviceList(devices.data(), &num_devices, sizeof(RAWINPUTDEVICELIST));

    int mouse_count = 0;
    for (UINT i = 0; i < num_devices; ++i) {
        if (devices[i].dwType == RIM_TYPEMOUSE) {
            ++mouse_count;
            char name[256] = {};
            UINT name_size = sizeof(name);
            if (GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICENAME, name, &name_size) > 0) {
                RID_DEVICE_INFO info{};
                UINT info_size = sizeof(info);
                info.cbSize = sizeof(info);
                GetRawInputDeviceInfoA(devices[i].hDevice, RIDI_DEVICEINFO, &info, &info_size);
                xlog::info("Raw input: mouse device #{}: id={} buttons={} sampleRate={} path={}",
                    mouse_count,
                    info.mouse.dwId,
                    info.mouse.dwNumberOfButtons,
                    info.mouse.dwSampleRate,
                    name);
            }
        }
    }
    xlog::info("Raw input: {} mouse device(s) detected out of {} total HID devices", mouse_count, num_devices);
}

static LRESULT CALLBACK raw_input_wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param)
{
    if (msg == WM_INPUT) {
        ++ri_event_count;
        // RIDEV_INPUTSINK delivers input regardless of focus, so drop it while the game is in background.
        if (ri_focused) {
            // One record per message. GetRawInputBuffer is deliberately not used: under WOW64 it
            // returns records with the 64-bit RAWINPUTHEADER layout (24 bytes) which breaks the
            // 32-bit struct definitions. Mouse records have a fixed size, so a stack RAWINPUT suffices.
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l_param), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
                && raw.header.dwType == RIM_TYPEMOUSE) {
                if (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                    // Absolute-only device: lLastX/lLastY are positions, not deltas. Record it once so
                    // the main thread can fall back; logging happens there.
                    if (!ri_absolute_seen) {
                        UINT name_size = sizeof(ri_absolute_device);
                        if (GetRawInputDeviceInfoA(raw.header.hDevice, RIDI_DEVICENAME, ri_absolute_device, &name_size) <= 0) {
                            strcpy_s(ri_absolute_device, "(unknown/injected device)");
                        }
                        ri_absolute_seen = true;
                    }
                }
                else {
                    AcquireSRWLockExclusive(&ri_lock);
                    ri_accum_dx += raw.data.mouse.lLastX;
                    ri_accum_dy += raw.data.mouse.lLastY;
                    ReleaseSRWLockExclusive(&ri_lock);
                }
            }
        }
    }
    return DefWindowProcA(hwnd, msg, w_param, l_param);
}

static DWORD WINAPI raw_input_thread_proc(LPVOID)
{
    // Create message-only window
    WNDCLASSA wc{};
    wc.lpfnWndProc = raw_input_wnd_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AFRawInputClass";
    RegisterClassA(&wc);

    ri_msg_wnd = CreateWindowA("AFRawInputClass", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!ri_msg_wnd) {
        xlog::error("Failed to create raw input message window");
        SetEvent(ri_init_event);
        return 1;
    }

    // Register for raw mouse input
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;     // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = ri_msg_wnd;

    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        xlog::error("Raw input: RegisterRawInputDevices failed (error {})", GetLastError());
        DestroyWindow(ri_msg_wnd);
        ri_msg_wnd = nullptr;
        SetEvent(ri_init_event);
        return 1;
    }

    xlog::info("Raw input: registered for mouse input on message-only window {:p}", static_cast<void*>(ri_msg_wnd));
    log_raw_mouse_devices();

    ri_event_count = 0;
    ri_absolute_seen = false;
    ri_running = true;
    SetEvent(ri_init_event);

    // Message loop
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    // Unregister
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    DestroyWindow(ri_msg_wnd);
    ri_msg_wnd = nullptr;
    ri_running = false;
    return 0;
}

void raw_input_start()
{
    if (ri_running)
        return;

    ri_init_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ri_init_event)
        return;

    ri_thread_handle = CreateThread(nullptr, 0, raw_input_thread_proc, nullptr, 0, &ri_thread_id);
    if (ri_thread_handle) {
        // Wait for thread to finish initialization
        WaitForSingleObject(ri_init_event, 3000);
        if (!ri_running) {
            WaitForSingleObject(ri_thread_handle, 1000);
            CloseHandle(ri_thread_handle);
            ri_thread_handle = nullptr;
        }
    }
    CloseHandle(ri_init_event);
    ri_init_event = nullptr;
}

void raw_input_stop()
{
    if (!ri_running || !ri_thread_handle)
        return;

    xlog::info("Raw input: stopping (received {} WM_INPUT events)", ri_event_count.load());

    PostThreadMessageA(ri_thread_id, WM_QUIT, 0, 0);
    if (WaitForSingleObject(ri_thread_handle, 3000) == WAIT_TIMEOUT) {
        xlog::warn("Raw input: thread did not exit within 3s timeout");
    }
    CloseHandle(ri_thread_handle);
    ri_thread_handle = nullptr;
}

bool raw_input_is_running()
{
    return ri_running;
}

void raw_input_consume_deltas(int& dx, int& dy)
{
    AcquireSRWLockExclusive(&ri_lock);
    dx = ri_accum_dx;
    dy = ri_accum_dy;
    ri_accum_dx = 0;
    ri_accum_dy = 0;
    ReleaseSRWLockExclusive(&ri_lock);
}

int raw_input_get_event_count()
{
    return ri_event_count;
}

void raw_input_set_focused(bool focused)
{
    ri_focused = focused;
}

const char* raw_input_absolute_device()
{
    return ri_absolute_seen ? ri_absolute_device : nullptr;
}
