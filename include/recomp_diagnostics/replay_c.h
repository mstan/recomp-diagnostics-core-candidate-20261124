#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompReplayWriter RecompReplayWriter;
typedef enum RecompReplayFormat {
 RECOMP_REPLAY_HASHED64=0, RECOMP_REPLAY_GENESIS32=1, RECOMP_REPLAY_SNES32=2
} RecompReplayFormat;
typedef struct RecompReplaySidecar {const char *source; const char *dest;} RecompReplaySidecar;
typedef struct RecompReplayPart {const void *data; size_t size; bool reuse;} RecompReplayPart;
RecompReplayWriter *recomp_replay_create(void);
/* Explicit format/capacity for existing recorder policies. UTF-8 paths on
 * Windows; zero capacity selects the format default, otherwise up to 32 MiB. */
RecompReplayWriter *recomp_replay_create_profile(RecompReplayFormat,size_t queue_bytes);
bool recomp_replay_begin(RecompReplayWriter *,const char *,const char *,const char *,const char *,const char *);
bool recomp_replay_begin_with_sidecars(RecompReplayWriter *,const char *path,const char *header,
 const RecompReplaySidecar *copies,size_t copy_count,
 const RecompReplaySidecar *inline_texts,size_t inline_count);
bool recomp_replay_begin_files(RecompReplayWriter *,const char *path,const char *header,
 const char *config_path,const char *mods_path,const char *effective_text);
bool recomp_replay_snapshot(RecompReplayWriter *,const void *,size_t);
bool recomp_replay_snapshot_raw(RecompReplayWriter *,const void *,size_t);
bool recomp_replay_line(RecompReplayWriter *,const char *);
bool recomp_replay_frame_parts(RecompReplayWriter *,uint64_t,const RecompReplayPart *,size_t count);
bool recomp_replay_hashes(RecompReplayWriter *,unsigned,const uint32_t[4]);
bool recomp_replay_remove(RecompReplayWriter *,const char *);
bool recomp_replay_end(RecompReplayWriter *);
const char *recomp_replay_error(RecompReplayWriter *);
const char *recomp_replay_destroy(RecompReplayWriter *);
#ifdef __cplusplus
}
#endif
