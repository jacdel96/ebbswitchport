// Async on-SD telemetry — periodic HUD metrics (and anything else worth
// keeping) appended to sdmc:/switch/ebbswitchport/telemetry.log by a
// background writer thread, so numbers can be read back from the card
// afterwards instead of photographed off the TV. Same async-writer shape as
// save_io.c: producers never touch the filesystem; a low-priority thread
// drains a ring of lines every ~500ms. Lines carry a seconds-since-launch
// timestamp prefix.
#pragma once

// Truncates any previous log and starts the writer thread. Safe to call
// even if the writer thread fails to start — telemetry_printf just becomes
// a cheap no-op-ish (lines still buffer, ring overwrites oldest).
void telemetry_init(void);

// printf into the ring. Thread-safe, never performs I/O on the calling
// thread, never blocks beyond the ring mutex. On overflow the oldest
// unwritten line is dropped — telemetry is best-effort by design.
void telemetry_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Flushes what's buffered and stops the writer.
void telemetry_exit(void);
