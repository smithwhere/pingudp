/* UDP echo probe client for Windows. The Linux peer echoes 16-byte UPNG packets. */
#define _WIN32_WINNT 0x0601
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET socket_t;
#define BAD_SOCKET INVALID_SOCKET
#define socket_close closesocket
#else
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int socket_t;
#define BAD_SOCKET (-1)
#define socket_close close
#define SOCKET_ERROR (-1)
#endif
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted = 0;
static void on_interrupt(int unused) { (void)unused; interrupted = 1; }

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static void pause_ms(unsigned milliseconds) {
#ifdef _WIN32
    Sleep(milliseconds);
#else
    struct timespec ts = {milliseconds / 1000, (long)(milliseconds % 1000) * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static int socket_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static void usage(const char *name) {
    fprintf(stderr, "Usage: %s HOST:PORT [-n COUNT | -t] [-i INTERVAL_MS] [-w TIMEOUT_MS]\n"
                    "  -n  Number of probes (default: 4)\n"
                    "  -t  Run until Ctrl+C\n"
                    "  -i  Minimum interval between sends (default: 1000 ms)\n"
                    "  -w  Reply timeout (default: 2000 ms)\n"
                    "Example: %s 203.0.113.10:9000 -t\n", name, name);
}

static int parse_number(const char *s, unsigned min, unsigned max, unsigned *out) {
    char *end;
    unsigned long value;
    if (!s || !*s || *s == '-') return 0;
    value = strtoul(s, &end, 10);
    if (*end || value < min || value > max) return 0;
    *out = (unsigned)value;
    return 1;
}

int main(int argc, char **argv) {
    unsigned count = 4, interval = 1000, timeout = 2000, port;
    int continuous = 0, specified_count = 0, i, rc = 0;
    char host[256], service[8], ip[INET_ADDRSTRLEN], *colon;
    struct addrinfo hints, *resolved = NULL;
    struct sockaddr_in target;
    socket_t sock = BAD_SOCKET;
    unsigned long sent = 0, received = 0, timed_out = 0, errors = 0;
    double min_rtt = 0, max_rtt = 0, total_rtt = 0;
    uint32_t session[2];
#ifdef _WIN32
    WSADATA startup;
    if (WSAStartup(MAKEWORD(2, 2), &startup) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
#endif
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(argv[0]); rc = argc < 2 ? 2 : 0; goto done;
    }
    colon = strrchr(argv[1], ':');
    if (!colon || colon == argv[1] || strchr(colon + 1, ':') ||
        (size_t)(colon - argv[1]) >= sizeof(host) ||
        !parse_number(colon + 1, 1, 65535, &port)) {
        fprintf(stderr, "Expected IPv4 address or hostname followed by :PORT\n");
        rc = 2; goto done;
    }
    memcpy(host, argv[1], (size_t)(colon - argv[1]));
    host[colon - argv[1]] = '\0';
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) continuous = 1;
        else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "-i") || !strcmp(argv[i], "-w")) && i + 1 < argc) {
            const char *option = argv[i++];
            unsigned *destination = !strcmp(option, "-n") ? &count : !strcmp(option, "-i") ? &interval : &timeout;
            if (!parse_number(argv[i], !strcmp(option, "-i") ? 10 : 1, !strcmp(option, "-n") ? 1000000000U : 60000U, destination)) {
                fprintf(stderr, "Invalid value for %s\n", option); rc = 2; goto done;
            }
            if (!strcmp(option, "-n")) specified_count = 1;
        } else { usage(argv[0]); rc = 2; goto done; }
    }
    if (continuous && specified_count) { fprintf(stderr, "Choose either -n or -t\n"); rc = 2; goto done; }
    snprintf(service, sizeof(service), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(host, service, &hints, &resolved) != 0) {
        fprintf(stderr, "Could not resolve %s\n", host); rc = 2; goto done;
    }
    memcpy(&target, resolved->ai_addr, sizeof(target));
    inet_ntop(AF_INET, &target.sin_addr, ip, sizeof(ip));
    freeaddrinfo(resolved); resolved = NULL;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == BAD_SOCKET) { fprintf(stderr, "socket failed: %d\n", socket_error()); rc = 2; goto done; }
    signal(SIGINT, on_interrupt);
    session[0] = (uint32_t)now_ms() ^ (uint32_t)(uintptr_t)&sock;
#ifdef _WIN32
    session[1] = (uint32_t)GetCurrentProcessId() ^ (uint32_t)GetTickCount64();
#else
    session[1] = (uint32_t)getpid() ^ (uint32_t)time(NULL);
#endif
    printf("UDP echo probing %s (%s):%u; timeout %u ms\n", host, ip, port, timeout);
    fflush(stdout);
    while (!interrupted && (continuous || sent < count)) {
        unsigned char packet[16] = {'U', 'P', 'N', 'G'}, reply[256];
        uint32_t seq = (uint32_t)(sent + 1);
        double started, deadline;
        int outcome = 0; /* 1: reply; 2: timeout; 3: socket error */
        memcpy(packet + 4, session, sizeof(session));
        memcpy(packet + 12, &seq, sizeof(seq));
        started = now_ms();
        sent++;
        if (sendto(sock, (const char *)packet, sizeof(packet), 0, (const struct sockaddr *)&target, sizeof(target)) == SOCKET_ERROR) {
            printf("seq=%lu send failed: %d\n", sent, socket_error()); errors++; outcome = 3;
        }
        deadline = started + timeout;
        while (!outcome && !interrupted) {
            fd_set readers;
            struct timeval wait;
            double remaining = deadline - now_ms();
            int ready;
            if (remaining <= 0) { outcome = 2; break; }
            if (remaining > 200.0) remaining = 200.0; /* allow Ctrl+C to finish promptly */
            wait.tv_sec = (long)(remaining / 1000.0);
            wait.tv_usec = (long)((remaining - wait.tv_sec * 1000.0) * 1000.0);
            FD_ZERO(&readers); FD_SET(sock, &readers);
#ifdef _WIN32
            ready = select(0, &readers, NULL, NULL, &wait);
#else
            ready = select(sock + 1, &readers, NULL, NULL, &wait);
#endif
            if (ready == 0) continue;
            if (ready == SOCKET_ERROR) {
                int err = socket_error();
#ifdef _WIN32
                if (err == WSAEINTR) continue;
#else
                if (err == EINTR) continue;
#endif
                printf("seq=%lu receive error: %d\n", sent, err); errors++; outcome = 3; break;
            }
            if (FD_ISSET(sock, &readers)) {
                struct sockaddr_in from;
#ifdef _WIN32
                int from_len = sizeof(from);
                int size = recvfrom(sock, (char *)reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_len);
#else
                socklen_t from_len = sizeof(from);
                int size = (int)recvfrom(sock, reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_len);
#endif
                if (size == SOCKET_ERROR) {
                    printf("seq=%lu receive error: %d\n", sent, socket_error()); errors++; outcome = 3; break;
                }
                if (from_len < (int)sizeof(from) || from.sin_family != AF_INET ||
                    from.sin_addr.s_addr != target.sin_addr.s_addr || from.sin_port != target.sin_port ||
                    size != (int)sizeof(packet) || memcmp(reply, packet, sizeof(packet))) continue;
                {
                    double rtt = now_ms() - started;
                    if (!received || rtt < min_rtt) min_rtt = rtt;
                    if (!received || rtt > max_rtt) max_rtt = rtt;
                    total_rtt += rtt; received++;
                    printf("Reply from %s:%u: seq=%lu time=%.2f ms\n", ip, port, sent, rtt);
                    outcome = 1;
                }
            }
        }
        if (outcome == 2) { printf("Request timed out: seq=%lu\n", sent); timed_out++; }
        fflush(stdout);
        if (!interrupted && (continuous || sent < count)) {
            double due = started + interval;
            while (!interrupted && now_ms() < due) {
                double remaining = due - now_ms();
                pause_ms((unsigned)(remaining > 50 ? 50 : remaining < 1 ? 1 : remaining));
            }
        }
    }
    printf("\nStatistics for %s:%u: sent=%lu received=%lu unanswered=%lu (%.1f%%), timeouts=%lu errors=%lu\n",
           ip, port, sent, received, sent - received,
           sent ? 100.0 * (sent - received) / sent : 0.0, timed_out, errors);
    if (received) printf("RTT min/avg/max = %.2f/%.2f/%.2f ms\n", min_rtt, total_rtt / received, max_rtt);
    rc = received ? 0 : 1;
done:
    if (resolved) freeaddrinfo(resolved);
    if (sock != BAD_SOCKET) socket_close(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
