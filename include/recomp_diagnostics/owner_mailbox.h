/* SPDX-License-Identifier: MIT */
#ifndef RECOMP_DIAGNOSTICS_OWNER_MAILBOX_H
#define RECOMP_DIAGNOSTICS_OWNER_MAILBOX_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rd_owner_mailbox rd_owner_mailbox;
typedef struct rd_mailbox_reply rd_mailbox_reply;
typedef struct rd_mailbox_result rd_mailbox_result;
enum {
  RD_MAILBOX_COMPLETED,
  RD_MAILBOX_STOPPED,
  RD_MAILBOX_BUSY,
  RD_MAILBOX_REQUEST_TOO_LARGE,
  RD_MAILBOX_TIMED_OUT,
  RD_MAILBOX_FAILED
};
typedef void (*rd_mailbox_handler)(void *user, const char *request,
                                   rd_mailbox_reply *reply);
rd_owner_mailbox *rd_owner_mailbox_create_psx(void);
void rd_owner_mailbox_stop(rd_owner_mailbox *mailbox);
void rd_owner_mailbox_destroy(rd_owner_mailbox *mailbox);
rd_mailbox_result *rd_owner_mailbox_call(rd_owner_mailbox *mailbox,
                                         const char *request,
                                         uint32_t transport_wait_ms);
int rd_owner_mailbox_poll(rd_owner_mailbox *mailbox, rd_mailbox_handler handler,
                          void *user);
int rd_mailbox_reply_line(rd_mailbox_reply *reply, const char *line);
int rd_mailbox_reply_overflow(const rd_mailbox_reply *reply);
int rd_mailbox_result_status(const rd_mailbox_result *result);
const char *rd_mailbox_result_data(const rd_mailbox_result *result);
size_t rd_mailbox_result_size(const rd_mailbox_result *result);
void rd_mailbox_result_destroy(rd_mailbox_result *result);
#ifdef __cplusplus
}
#endif
#endif
