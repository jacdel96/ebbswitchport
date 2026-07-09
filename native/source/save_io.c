#include "save_io.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#define MAX_SLOTS 4  // generous headroom; only 3 distinct paths ever used (srm/auto1/auto10)

typedef struct {
    bool pending;
    char path[256];
    void *data;
    size_t size;
} SaveSlot;

static SaveSlot g_slots[MAX_SLOTS];
static Mutex g_lock;
static Thread g_thread;
static volatile bool g_run = false;
static bool g_async_ok = false;  // false => write_atomic runs inline (thread failed to start)

static void write_atomic(const char *path, const void *data, size_t size) {
    char tmp[264];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
    remove(path);
    rename(tmp, path);
}

static void writer_thread(void *arg) {
    (void)arg;
    while (g_run) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            mutexLock(&g_lock);
            bool has_job = g_slots[i].pending;
            char path[256] = {0};
            void *data = NULL;
            size_t size = 0;
            if (has_job) {
                memcpy(path, g_slots[i].path, sizeof(path));
                data = g_slots[i].data;
                size = g_slots[i].size;
                g_slots[i].pending = false;
                g_slots[i].data = NULL;
            }
            mutexUnlock(&g_lock);
            if (has_job) {
                write_atomic(path, data, size);
                free(data);
            }
        }
        svcSleepThread(50000000ULL);  // 50ms; saves are seconds apart, no rush
    }
}

void save_io_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    mutexInit(&g_lock);
    g_run = true;
    if (R_FAILED(threadCreate(&g_thread, writer_thread, NULL, NULL, 0x4000, 0x3B, -2)) ||
        R_FAILED(threadStart(&g_thread))) {
        g_run = false;
        g_async_ok = false;
        return;
    }
    g_async_ok = true;
}

void save_io_write_async(const char *path, void *data, size_t size) {
    if (!g_async_ok) {
        write_atomic(path, data, size);
        free(data);
        return;
    }

    mutexLock(&g_lock);
    int slot = -1;
    // Reuse a slot already queued for this same path (only the latest matters).
    for (int i = 0; i < MAX_SLOTS; i++)
        if (g_slots[i].pending && strcmp(g_slots[i].path, path) == 0) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < MAX_SLOTS; i++)
            if (!g_slots[i].pending) { slot = i; break; }
    if (slot < 0) slot = 0;  // all busy (shouldn't happen: only 3 distinct paths) — drop oldest

    free(g_slots[slot].data);  // no-op if NULL; frees a superseded/dropped pending buffer
    snprintf(g_slots[slot].path, sizeof(g_slots[slot].path), "%s", path);
    g_slots[slot].data = data;
    g_slots[slot].size = size;
    g_slots[slot].pending = true;
    mutexUnlock(&g_lock);
}

void save_io_flush(void) {
    if (!g_async_ok) return;
    for (;;) {
        mutexLock(&g_lock);
        bool any = false;
        for (int i = 0; i < MAX_SLOTS; i++) if (g_slots[i].pending) any = true;
        mutexUnlock(&g_lock);
        if (!any) return;
        svcSleepThread(5000000ULL);  // 5ms
    }
}

void save_io_exit(void) {
    save_io_flush();
    if (!g_async_ok) return;
    g_run = false;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
}
