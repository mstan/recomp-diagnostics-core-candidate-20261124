/* SPDX-License-Identifier: MIT */
#ifndef RECOMP_DIAGNOSTICS_OPAQUE_WRITER_H
#define RECOMP_DIAGNOSTICS_OPAQUE_WRITER_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rd_opaque_writer rd_opaque_writer;
typedef struct rd_opaque_sink rd_opaque_sink;
enum { RD_OPAQUE_PSP, RD_OPAQUE_DREAMCAST, RD_OPAQUE_PS2 };
typedef struct {
  const char *name;
  const void *header;
  size_t header_bytes;
  int exclusive;
} rd_opaque_stream;
typedef int (*rd_opaque_encode)(void *record, rd_opaque_sink *sink);
typedef void (*rd_opaque_release)(void *record);
rd_opaque_writer *rd_opaque_writer_create(int profile);
int rd_opaque_writer_open(rd_opaque_writer *, const char *directory,
                          const rd_opaque_stream *, size_t count,
                          uint64_t initial_bytes);
/* Always consumes record, including on admission/allocation failure. encode
 * runs only on the shared worker; release runs exactly once. Do not reenter
 * the writer from encode/release. The borrowed sink lives for this call only.
 */
int rd_opaque_writer_push(rd_opaque_writer *, void *record, rd_opaque_encode,
                          rd_opaque_release, size_t queue_bytes,
                          uint64_t admitted_bytes, uint64_t frames);
void rd_opaque_writer_close(rd_opaque_writer *);
void rd_opaque_writer_request_stop(rd_opaque_writer *);
void rd_opaque_writer_join(rd_opaque_writer *);
void rd_opaque_writer_destroy(rd_opaque_writer *);
int rd_opaque_writer_good(const rd_opaque_writer *);
const char *rd_opaque_writer_error(const rd_opaque_writer *);
uint64_t rd_opaque_writer_written(const rd_opaque_writer *);
int rd_opaque_sink_write(rd_opaque_sink *, unsigned stream, const void *bytes,
                         size_t size);
int rd_opaque_sink_charge_auxiliary(rd_opaque_sink *, size_t size);
/* classify:0 unowned,1 protected,2 reclaimable,-1 native parser/I/O error.
 * allowed receives a relative UTF-8 path and directory flag. No callback may
 * delete files; the provider validates the whole tree before removing any. */
typedef int (*rd_history_classify)(void *user, const char *path);
typedef int (*rd_history_allowed)(void *user, const char *relative,
                                  int directory);
int rd_owned_history_reserve(const char *root, unsigned sessions,
                             unsigned directory_depth, int protect_readonly,
                             rd_history_classify, rd_history_allowed,
                             void *user, int *os_error);
int rd_owned_history_remove(const char *path, unsigned directory_depth,
                            int protect_readonly, rd_history_allowed,
                            void *user, int *os_error);
typedef struct rd_history_admission rd_history_admission;
rd_history_admission *rd_history_admission_acquire(const char *path,
                                                   int *os_error);
void rd_history_admission_release(rd_history_admission *);
#ifdef __cplusplus
}
#endif
#endif
