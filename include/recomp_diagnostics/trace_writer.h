/* SPDX-License-Identifier: MIT */
#ifndef RECOMP_DIAGNOSTICS_TRACE_WRITER_H
#define RECOMP_DIAGNOSTICS_TRACE_WRITER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompTraceWriter RecompTraceWriter;
typedef struct RecompTraceHost {
    void *user;
    /* Host timing/lifecycle only: never access guest state from these hooks.
     * They must not throw or reenter the writer. Keep user alive through close. */
    uint32_t (*ticks_ms)(void *);
    void (*sleep_ms)(void *, unsigned);
    void (*worker_ready)(void *);
} RecompTraceHost;
typedef struct RecompTraceOptions {
    size_t size, queue_bytes, row_bytes, buffer_bytes, segment_bytes;
    unsigned keep_segments, poll_ms, flush_ms;
    RecompTraceHost host;
} RecompTraceOptions;
/* Defaults preserve the qualified 4 MiB byte-ring / 8 KiB row profile.
 * Queue capacity must be a power of two; one byte is reserved. Each row is
 * charged its exact byte length plus four bytes, released after worker copy.
 * Segment bytes include the header; rotate before the next row at the limit. */
RecompTraceOptions recomp_trace_options(size_t segment_bytes, unsigned keep);
/* One producer; stop it before close. Owned row bytes cross the byte queue.
 * No producer allocation, lock, file I/O or wait. Overflow stops admission and
 * drains the accepted prefix. The first error wins. open copies stem/header;
 * file-open errors are asynchronous, while allocation/start errors return NULL.
 * Names are stem-NNN.csv and retention removes only the corresponding old name.
 * Paths use the native stdio convention of the qualified profile. */
RecompTraceWriter *recomp_trace_open(const char *stem, const char *header,
                                    const RecompTraceOptions *options);
int recomp_trace_write(RecompTraceWriter *, const void *row, size_t size);
/* Producer-side diagnostic gauge; not an admission reservation. */
size_t recomp_trace_free_bytes(const RecompTraceWriter *);
const char *recomp_trace_error(const RecompTraceWriter *);
/* Drains accepted rows and joins without an elapsed cutoff. The returned error
 * is static text and remains valid after destruction. NULL handles are safe. */
const char *recomp_trace_close(RecompTraceWriter *);
#ifdef __cplusplus
}
#endif
#endif
