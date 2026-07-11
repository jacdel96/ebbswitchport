#include "telemetry.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#define LOG_PATH "sdmc:/switch/ebbswitchport/telemetry.log"
#define RING_LINES 64
#define LINE_LEN 192

static char g_ring[RING_LINES][LINE_LEN];
static unsigned g_head, g_count;   // g_lock-guarded
static Mutex g_lock;
static Thread g_thread;
static volatile bool g_run = false;
static bool g_ok = false;
static u64 g_t0;

static void flush_ring(void) {
    // Drain under the lock into a local copy, then do the file I/O with the
    // lock released — producers never wait on the SD card.
    static char local[RING_LINES][LINE_LEN];  // static: this thread's stack is small
    unsigned n = 0;
    mutexLock(&g_lock);
    while (g_count > 0 && n < RING_LINES) {
        memcpy(local[n++], g_ring[g_head], LINE_LEN);
        g_head = (g_head + 1) % RING_LINES;
        g_count--;
    }
    mutexUnlock(&g_lock);
    if (n == 0) return;

    // Open-append-close per flush: costs a little more than keeping the file
    // open, but every flushed line is durable even if the app crashes —
    // which is exactly when a telemetry log matters most.
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    for (unsigned i = 0; i < n; i++) {
        fputs(local[i], f);
        fputc('\n', f);
    }
    fclose(f);
}

static void writer_thread(void *arg) {
    (void)arg;
    while (g_run) {
        flush_ring();
        svcSleepThread(500000000ULL);  // 500ms — telemetry cadence, no rush
    }
    flush_ring();  // final drain on shutdown
}

void telemetry_init(void) {
    mutexInit(&g_lock);
    g_head = g_count = 0;
    g_t0 = armGetSystemTick();

    FILE *f = fopen(LOG_PATH, "w");  // truncate the previous session's log
    if (f) fclose(f);

    g_run = true;
    if (R_FAILED(threadCreate(&g_thread, writer_thread, NULL, NULL, 0x4000, 0x3B, -2)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_run = false;
        g_ok = false;
        return;
    }
    g_ok = true;
}

void telemetry_printf(const char *fmt, ...) {
    char line[LINE_LEN];
    unsigned t_ds = (unsigned)(armTicksToNs(armGetSystemTick() - g_t0) / 100000000ULL);
    int off = snprintf(line, sizeof(line), "[%u.%us] ", t_ds / 10, t_ds % 10);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + off, sizeof(line) - (size_t)off, fmt, ap);
    va_end(ap);

    mutexLock(&g_lock);
    unsigned slot = (g_head + g_count) % RING_LINES;
    if (g_count == RING_LINES) {           // full — drop the oldest
        g_head = (g_head + 1) % RING_LINES;
        g_count--;
        slot = (g_head + g_count) % RING_LINES;
    }
    memcpy(g_ring[slot], line, LINE_LEN);
    g_ring[slot][LINE_LEN - 1] = '\0';
    g_count++;
    mutexUnlock(&g_lock);
}

void telemetry_exit(void) {
    if (!g_ok) return;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}
