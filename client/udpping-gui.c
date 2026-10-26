#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WM_LOG_LINE (WM_APP + 1)
#define WM_STATUS_TEXT (WM_APP + 2)
#define WM_WORK_DONE (WM_APP + 3)
#define ID_START 101
#define ID_STOP 102

static HWND window, host_edit, port_edit, count_edit, interval_edit, timeout_edit;
static HWND start_button, stop_button, output_edit, status_label;
static HANDLE stop_event, worker_handle;
static BOOL closing;
static HFONT ui_font;

typedef struct {
    WCHAR host[256];
    unsigned port, count, interval, timeout;
} Settings;

static double clock_ms(void) {
    LARGE_INTEGER tick, frequency;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&frequency);
    return (double)tick.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

/* SendMessage copies text into a control before this stack buffer is released. */
static void show_text(UINT message, const char *format, ...) {
    char utf8[512];
    WCHAR wide[512];
    va_list args;
    va_start(args, format);
    vsnprintf(utf8, sizeof(utf8), format, args);
    va_end(args);
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, 512)) return;
    SendMessageW(window, message, 0, (LPARAM)wide);
}

static unsigned __stdcall probe_thread(void *argument) {
    Settings *settings = (Settings *)argument;
    ADDRINFOW hints = {0}, *results = NULL;
    SOCKADDR_IN destination;
    SOCKET sock = INVALID_SOCKET;
    WCHAR service[8], ip[INET_ADDRSTRLEN];
    uint32_t token[2];
    unsigned long sent = 0, received = 0, timeouts = 0, errors = 0;
    double min_rtt = 0, max_rtt = 0, total_rtt = 0;
    unsigned long seq = 0;
    int lookup;

    swprintf(service, 8, L"%u", settings->port);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    lookup = GetAddrInfoW(settings->host, service, &hints, &results);
    if (lookup != 0) {
        show_text(WM_LOG_LINE, "Could not resolve host (error %d).", lookup);
        show_text(WM_STATUS_TEXT, "DNS lookup failed");
        goto finish;
    }
    memcpy(&destination, results->ai_addr, sizeof(destination));
    FreeAddrInfoW(results);
    results = NULL;
    if (!InetNtopW(AF_INET, &destination.sin_addr, ip, INET_ADDRSTRLEN)) {
        show_text(WM_LOG_LINE, "Could not format target address.");
        goto finish;
    }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        show_text(WM_LOG_LINE, "Socket error %d.", WSAGetLastError());
        goto finish;
    }
    token[0] = (uint32_t)GetTickCount64() ^ (uint32_t)(uintptr_t)&destination;
    token[1] = (uint32_t)GetCurrentProcessId() ^ (uint32_t)clock_ms();
    {
        char address[INET_ADDRSTRLEN];
        WideCharToMultiByte(CP_UTF8, 0, ip, -1, address, sizeof(address), NULL, NULL);
        show_text(WM_LOG_LINE, "UDP echo probe %s:%u (timeout %u ms)", address, settings->port, settings->timeout);
    }

    while (WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0 &&
           (settings->count == 0 || sent < settings->count)) {
        unsigned char packet[16] = {'U', 'P', 'N', 'G'}, reply[256];
        double started = clock_ms(), deadline;
        int outcome = 0; /* 1 valid reply, 2 timed out, 3 socket error */
        uint32_t packet_seq = ++seq;
        sent++;
        memcpy(packet + 4, token, sizeof(token));
        memcpy(packet + 12, &packet_seq, sizeof(packet_seq));
        if (sendto(sock, (const char *)packet, sizeof(packet), 0,
                   (const SOCKADDR *)&destination, sizeof(destination)) == SOCKET_ERROR) {
            show_text(WM_LOG_LINE, "seq=%lu send error %d", seq, WSAGetLastError());
            errors++; outcome = 3;
        }
        deadline = started + settings->timeout;
        while (!outcome && WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
            double remaining = deadline - clock_ms();
            struct timeval wait;
            fd_set readers;
            int ready;
            if (remaining <= 0) { outcome = 2; break; }
            if (remaining > 100) remaining = 100;
            wait.tv_sec = 0;
            wait.tv_usec = (long)(remaining * 1000.0);
            FD_ZERO(&readers);
            FD_SET(sock, &readers);
            ready = select(0, &readers, NULL, NULL, &wait);
            if (ready == 0) continue;
            if (ready == SOCKET_ERROR) {
                show_text(WM_LOG_LINE, "seq=%lu receive error %d", seq, WSAGetLastError());
                errors++; outcome = 3; break;
            }
            if (FD_ISSET(sock, &readers)) {
                SOCKADDR_IN from;
                int from_size = sizeof(from);
                int length = recvfrom(sock, (char *)reply, sizeof(reply), 0,
                                      (SOCKADDR *)&from, &from_size);
                if (length == SOCKET_ERROR) {
                    show_text(WM_LOG_LINE, "seq=%lu receive error %d", seq, WSAGetLastError());
                    errors++; outcome = 3; break;
                }
                if (from_size < (int)sizeof(from) || from.sin_family != AF_INET ||
                    from.sin_addr.s_addr != destination.sin_addr.s_addr ||
                    from.sin_port != destination.sin_port ||
                    length != (int)sizeof(packet) || memcmp(reply, packet, sizeof(packet))) continue;
                {
                    double rtt = clock_ms() - started;
                    if (!received || rtt < min_rtt) min_rtt = rtt;
                    if (!received || rtt > max_rtt) max_rtt = rtt;
                    received++;
                    total_rtt += rtt;
                    show_text(WM_LOG_LINE, "seq=%lu reply: %.2f ms", seq, rtt);
                    outcome = 1;
                }
            }
        }
        if (outcome == 2) { show_text(WM_LOG_LINE, "seq=%lu request timed out", seq); timeouts++; }
        if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
        if (settings->count && sent >= settings->count) break;
        {
            double until = started + settings->interval - clock_ms();
            if (until > 0) WaitForSingleObject(stop_event, (DWORD)until);
        }
    }
    show_text(WM_LOG_LINE, "--- sent=%lu received=%lu unanswered=%lu (%.1f%%); timeouts=%lu errors=%lu ---",
              sent, received, sent - received,
              sent ? 100.0 * (sent - received) / sent : 0.0, timeouts, errors);
    if (received) show_text(WM_LOG_LINE, "RTT min/avg/max: %.2f / %.2f / %.2f ms",
                            min_rtt, total_rtt / received, max_rtt);
    show_text(WM_STATUS_TEXT, "Sent %lu  Received %lu  Unanswered %.1f%%  Avg %.2f ms",
              sent, received, sent ? 100.0 * (sent - received) / sent : 0.0,
              received ? total_rtt / received : 0.0);
finish:
    if (results) FreeAddrInfoW(results);
    if (sock != INVALID_SOCKET) closesocket(sock);
    free(settings);
    PostMessageW(window, WM_WORK_DONE, 0, 0);
    return 0;
}

static HWND control(const WCHAR *kind, const WCHAR *caption, DWORD style,
                    int x, int y, int width, int height, int id) {
    HWND h = CreateWindowExW(0, kind, caption, WS_CHILD | WS_VISIBLE | style,
                             x, y, width, height, window, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)ui_font, TRUE);
    return h;
}

static int read_number(HWND edit, unsigned min, unsigned max, unsigned *value) {
    WCHAR text[32], *end;
    unsigned long n;
    GetWindowTextW(edit, text, 32);
    if (!text[0] || text[0] == L'-') return 0;
    n = wcstoul(text, &end, 10);
    if (*end || n < min || n > max) return 0;
    *value = (unsigned)n;
    return 1;
}

static void enable_inputs(BOOL enabled) {
    EnableWindow(host_edit, enabled);
    EnableWindow(port_edit, enabled);
    EnableWindow(count_edit, enabled);
    EnableWindow(interval_edit, enabled);
    EnableWindow(timeout_edit, enabled);
    EnableWindow(start_button, enabled);
    EnableWindow(stop_button, !enabled);
}

static void start_probe(void) {
    Settings *settings;
    uintptr_t thread;
    if (worker_handle) return;
    settings = (Settings *)calloc(1, sizeof(*settings));
    if (!settings) { MessageBoxW(window, L"内存不足", L"UDP Ping", MB_ICONERROR); return; }
    GetWindowTextW(host_edit, settings->host, 256);
    if (!settings->host[0] || wcschr(settings->host, L':') ||
        !read_number(port_edit, 1, 65535, &settings->port) ||
        !read_number(count_edit, 0, 1000000000, &settings->count) ||
        !read_number(interval_edit, 10, 60000, &settings->interval) ||
        !read_number(timeout_edit, 1, 60000, &settings->timeout)) {
        MessageBoxW(window, L"请填写 IPv4 地址或域名，并检查端口、次数、间隔和超时。\n次数为 0 表示持续探测。",
                    L"输入无效", MB_ICONWARNING);
        free(settings);
        return;
    }
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_event) { free(settings); MessageBoxW(window, L"无法创建停止事件", L"UDP Ping", MB_ICONERROR); return; }
    SetWindowTextW(output_edit, L"");
    SetWindowTextW(status_label, L"正在探测...");
    enable_inputs(FALSE);
    thread = _beginthreadex(NULL, 0, probe_thread, settings, 0, NULL);
    if (!thread) {
        free(settings);
        CloseHandle(stop_event); stop_event = NULL;
        enable_inputs(TRUE);
        MessageBoxW(window, L"无法启动探测线程", L"UDP Ping", MB_ICONERROR);
    } else worker_handle = (HANDLE)thread;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        window = hwnd;
        ui_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        control(L"STATIC", L"VPS 地址", 0, 16, 20, 70, 24, 0);
        host_edit = control(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 90, 18, 292, 25, 0);
        control(L"STATIC", L"UDP 端口", 0, 397, 20, 68, 24, 0);
        port_edit = control(L"EDIT", L"9000", WS_BORDER | ES_NUMBER, 468, 18, 78, 25, 0);
        control(L"STATIC", L"次数 (0=持续)", 0, 559, 20, 100, 24, 0);
        count_edit = control(L"EDIT", L"0", WS_BORDER | ES_NUMBER, 660, 18, 65, 25, 0);
        control(L"STATIC", L"间隔 (ms)", 0, 16, 61, 82, 24, 0);
        interval_edit = control(L"EDIT", L"1000", WS_BORDER | ES_NUMBER, 103, 59, 80, 25, 0);
        control(L"STATIC", L"超时 (ms)", 0, 205, 61, 82, 24, 0);
        timeout_edit = control(L"EDIT", L"2000", WS_BORDER | ES_NUMBER, 291, 59, 80, 25, 0);
        start_button = control(L"BUTTON", L"开始", BS_DEFPUSHBUTTON, 482, 57, 110, 30, ID_START);
        stop_button = control(L"BUTTON", L"停止", 0, 607, 57, 118, 30, ID_STOP);
        EnableWindow(stop_button, FALSE);
        control(L"STATIC", L"逐次结果", 0, 16, 105, 120, 24, 0);
        output_edit = control(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                              ES_AUTOVSCROLL | ES_READONLY, 16, 130, 709, 348, 0);
        SendMessageW(output_edit, EM_SETLIMITTEXT, 1048576, 0);
        status_label = control(L"STATIC", L"输入 VPS 公网 IP，然后点击开始。", 0, 16, 494, 709, 28, 0);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_START) start_probe();
        else if (LOWORD(wparam) == ID_STOP && stop_event) SetEvent(stop_event);
        return 0;
    case WM_LOG_LINE: {
        WCHAR line[1024];
        int length;
        int lines = (int)SendMessageW(output_edit, EM_GETLINECOUNT, 0, 0);
        /* Keep the most recent 2000 lines during continuous probing. */
        if (lines > 2000) {
            LRESULT cut = SendMessageW(output_edit, EM_LINEINDEX, lines - 2000, 0);
            if (cut > 0) {
                SendMessageW(output_edit, EM_SETSEL, 0, cut);
                SendMessageW(output_edit, EM_REPLACESEL, FALSE, (LPARAM)L"");
            }
        }
        swprintf(line, 1024, L"%ls\r\n", (const WCHAR *)lparam);
        /* -1 as the start only deselects; use the actual text length to append. */
        length = GetWindowTextLengthW(output_edit);
        SendMessageW(output_edit, EM_SETSEL, (WPARAM)length, (LPARAM)length);
        SendMessageW(output_edit, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(output_edit, EM_SCROLLCARET, 0, 0);
        return 0;
    }
    case WM_STATUS_TEXT:
        SetWindowTextW(status_label, (const WCHAR *)lparam);
        return 0;
    case WM_WORK_DONE:
        if (worker_handle) { CloseHandle(worker_handle); worker_handle = NULL; }
        if (stop_event) { CloseHandle(stop_event); stop_event = NULL; }
        if (closing) DestroyWindow(hwnd);
        else enable_inputs(TRUE);
        return 0;
    case WM_CLOSE:
        if (worker_handle) {
            closing = TRUE;
            SetEvent(stop_event);
            EnableWindow(stop_button, FALSE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    WSADATA startup;
    WNDCLASSW klass = {0};
    MSG message;
    (void)previous; (void)command_line;
    if (WSAStartup(MAKEWORD(2, 2), &startup)) {
        MessageBoxW(NULL, L"Windows 网络初始化失败", L"UDP Ping", MB_ICONERROR);
        return 2;
    }
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.lpszClassName = L"UdpPingClientWindow";
    klass.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    klass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    klass.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassW(&klass)) { WSACleanup(); return 2; }
    window = CreateWindowExW(0, klass.lpszClassName, L"UDP Ping 1.0.1 - Linux VPS",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 755, 570,
                            NULL, NULL, instance, NULL);
    if (!window) { WSACleanup(); return 2; }
    ShowWindow(window, show);
    UpdateWindow(window);
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WSACleanup();
    return (int)message.wParam;
}
