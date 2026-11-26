/* Shared nonblocking line framing. Each server owns one instance. */
#ifndef VB_DEBUG_STREAM_H
#define VB_DEBUG_STREAM_H
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* input;
    size_t used,input_capacity;
    char* output;
    size_t size, sent;
    int failed,discard;
    size_t line_limit,output_limit;
    void (*invalid)(void*,const char*);void* invalid_user;
} DiagStream;
#define vb_stream (*stream)

static void vb_stream_reset(DiagStream* stream) {
    free(vb_stream.input);
    free(vb_stream.output);
    memset(&vb_stream, 0, sizeof(vb_stream));
}
static int vb_stream_reserve_input(DiagStream* stream,size_t needed,size_t limit) {
    if(needed<=vb_stream.input_capacity)return 1;
    size_t capacity=vb_stream.input_capacity?vb_stream.input_capacity:8192u;
    while(capacity<needed) {
        if(capacity>(limit+1u)/2u){capacity=limit+1u;break;}
        capacity*=2u;
    }
    if(capacity>limit+1u)capacity=limit+1u;
    if(capacity<needed)return 0;
    char* input=(char*)realloc(vb_stream.input,capacity);
    if(!input)return 0;
    vb_stream.input=input;vb_stream.input_capacity=capacity;return 1;
}
static void vb_stream_queue_bytes(DiagStream* stream,const char* body,size_t n) {
    if (vb_stream.failed) return;
    size_t limit=vb_stream.output_limit?vb_stream.output_limit:64u*1024u*1024u;
    if ((!body&&n) || n > limit || vb_stream.size > limit-n) {
        vb_stream.failed=1;return;
    }
    if(!n)return;
    char* next=(char*)realloc(vb_stream.output,vb_stream.size+n);
    if(!next){vb_stream.failed=1;return;}
    vb_stream.output=next;memcpy(next+vb_stream.size,body,n);vb_stream.size+=n;
}
static void vb_stream_queue(DiagStream* stream,const char* body) {
    vb_stream_queue_bytes(stream,body,strlen(body));
    vb_stream_queue_bytes(stream,"\n",1);
}
static int vb_stream_would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}
static int vb_stream_flush(DiagStream* stream, sock_t client) {
    if (vb_stream.failed) return 0;
    while (vb_stream.sent < vb_stream.size) {
        size_t remaining = vb_stream.size - vb_stream.sent;
        int n = send(client, vb_stream.output + vb_stream.sent,
                     (int)(remaining > 65536 ? 65536 : remaining),
#ifdef MSG_NOSIGNAL
                     MSG_NOSIGNAL
#else
                     0
#endif
        );
        if (n <= 0) return n < 0 && vb_stream_would_block();
        vb_stream.sent += (size_t)n;
    }
    free(vb_stream.output);
    vb_stream.output = NULL;
    vb_stream.size = vb_stream.sent = 0;
    return 1;
}
static int vb_stream_poll(DiagStream* stream, sock_t client, void (*dispatch)(void*,char*), void* user) {
    if (!vb_stream_flush(stream,client)) return 0;
    /* Apply backpressure until the previous responses have drained. */
    if (vb_stream.size) return 1;
    char chunk[4096];
    int n = recv(client, chunk, sizeof(chunk), 0);
    if (n <= 0) return n < 0 && vb_stream_would_block();
    for (int i = 0; i < n; ++i) {
        if (chunk[i] == '\n') {
            if(vb_stream.discard){vb_stream.discard=0;vb_stream.used=0;continue;}
            if (vb_stream.used && vb_stream.input[vb_stream.used - 1] == '\r')
                --vb_stream.used;
            size_t limit=vb_stream.line_limit?vb_stream.line_limit:8191u;
            if(!vb_stream_reserve_input(stream,vb_stream.used+1u,limit))return 0;
            vb_stream.input[vb_stream.used] = 0;
            if (vb_stream.used) dispatch(user,vb_stream.input);
            vb_stream.used = 0;
        } else {
            if(vb_stream.discard)continue;
            size_t limit=vb_stream.line_limit?vb_stream.line_limit:8191u;
            if(!chunk[i] || vb_stream.used==limit) {
                if(!vb_stream.invalid)return 0;
                vb_stream.invalid(vb_stream.invalid_user,!chunk[i]?"NUL byte in command line":"command line too long");
                vb_stream.used=0;vb_stream.discard=1;continue;
            }
            if(!vb_stream_reserve_input(stream,vb_stream.used+2u,limit))return 0;
            vb_stream.input[vb_stream.used++] = chunk[i];
        }
    }
    return vb_stream_flush(stream,client);
}
#endif

#undef vb_stream
