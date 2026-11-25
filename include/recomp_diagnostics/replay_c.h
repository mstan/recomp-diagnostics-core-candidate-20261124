#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompReplayWriter RecompReplayWriter;
RecompReplayWriter *recomp_replay_create(void);
bool recomp_replay_begin(RecompReplayWriter *,const char *,const char *,const char *,const char *,const char *);
bool recomp_replay_snapshot(RecompReplayWriter *,const void *,size_t);
bool recomp_replay_line(RecompReplayWriter *,const char *);
bool recomp_replay_hashes(RecompReplayWriter *,unsigned,const uint32_t[4]);
bool recomp_replay_remove(RecompReplayWriter *,const char *);
bool recomp_replay_end(RecompReplayWriter *);
const char *recomp_replay_error(RecompReplayWriter *);
const char *recomp_replay_destroy(RecompReplayWriter *);
#ifdef __cplusplus
}
#endif
