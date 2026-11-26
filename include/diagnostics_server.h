#pragma once
#include <stdbool.h>
#include <stddef.h>
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
/* Alternate complete-line dispatcher for an existing engine protocol. It
 * owns parsing; named-command dispatch is bypassed. Same callback rules. */
/* Configure a fixed raw-protocol limit/error profile before any client.
 * The default line limit remains8191 bytes. Explicit profiles allow up to
 *16MiB, grown on demand per connection; response hard bounds remain64MiB.
 * An error handler receives a malformed-line reason; that line is discarded
 * through its newline and the connection remains usable. Without it malformed
 * framing closes the client. Limits cannot exceed these hard bounds. */
bool diag_server_protocol(DiagServer*,size_t line_bytes,size_t response_bytes,
                           DiagCommand framing_error,void* user);
bool diag_server_raw_handler(DiagServer*,DiagCommand,void* user);
bool diag_server_write(DiagServer*,const char* bytes,size_t size);
bool diag_server_connected(const DiagServer*);
/* Deferred until the current poll returns; safe within a command callback. */
void diag_server_disconnect(DiagServer*);
bool diag_server_poll(DiagServer*);
/* One newline is appended. Responses are bounded to 64 MiB per connection. */
bool diag_server_reply(DiagServer*,const char* text);
#ifdef __cplusplus
}
#endif
