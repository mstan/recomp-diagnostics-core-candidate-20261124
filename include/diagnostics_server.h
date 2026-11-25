#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DiagServer DiagServer;
/* Callbacks may reply, but must not destroy, register commands or recursively
 * poll the same server. Arguments are borrowed until the callback returns. */
typedef void (*DiagCommand)(void* user,DiagServer*,const char* arguments);
/* Loopback-only, nonblocking. Port 0 requests an available port. Poll on the
   engine owner thread; callbacks never mutate guest state on a worker. */
DiagServer* diag_server_create(uint16_t port);
void diag_server_destroy(DiagServer*);
uint16_t diag_server_port(const DiagServer*);
bool diag_server_register(DiagServer*,const char* name,DiagCommand,void* user);
bool diag_server_poll(DiagServer*);
/* One newline is appended. Responses are bounded to 64 MiB per connection. */
bool diag_server_reply(DiagServer*,const char* text);
#ifdef __cplusplus
}
#endif
