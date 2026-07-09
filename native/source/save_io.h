// Background writer for save-state/SRAM persistence to the SD card.
//
// main.c captures a heap snapshot of SRAM/state data on the main thread (the
// only thread allowed to touch the libretro core) and hands ownership of it
// here; the actual slow SD-card I/O then happens off the main thread, so it
// never blocks retro_run()/audio_submit() — synchronous saves were previously
// starving the audio ring every ~10s (autosave interval) while the separate
// feeder thread kept draining it in real time, producing an audible sawtooth.
#pragma once
#include <stddef.h>

void save_io_init(void);

// Queues an atomic (tmp-file + rename) write of `data` to `path`. `data` must
// be heap-allocated (malloc/calloc); ownership transfers here and it is freed
// once written. A write already pending for the same `path` is superseded
// (only the latest snapshot of any given save file matters).
void save_io_write_async(const char *path, void *data, size_t size);

// Blocks until every queued write has actually landed on disk. Call before
// the process exits — an async write can't be left in flight past that point.
void save_io_flush(void);

void save_io_exit(void);
