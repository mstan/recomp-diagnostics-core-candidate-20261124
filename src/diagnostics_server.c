#include "diagnostics_server.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int sock_t;
#define BAD_SOCKET (-1)
#define close_socket close
#endif
#include "debug_stream.h"

typedef struct Command {
    struct Command* next;
    char* name;
    DiagCommand callback;
    void* user;
} Command;
struct DiagServer {
    sock_t listener, client;
    uint16_t port;
    DiagStream stream;
    Command* commands;
    DiagCommand raw;void* raw_user;int disconnect;
    size_t line_limit,output_limit;DiagCommand framing_error;void* framing_user;
};
static int nonblocking(sock_t fd) {
#ifdef _WIN32
    u_long enabled=1; return ioctlsocket(fd,FIONBIO,&enabled)==0;
#else
    int flags=fcntl(fd,F_GETFL,0);
    return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0;
#endif
}
DiagServer* diag_server_create(uint16_t port) {
#ifdef _WIN32
    WSADATA data;
    if(WSAStartup(MAKEWORD(2,2),&data)!=0)return NULL;
#endif
    DiagServer* s=calloc(1,sizeof(*s));
    if(!s) {
#ifdef _WIN32
        WSACleanup();
#endif
        return NULL;
    }
    s->client=BAD_SOCKET;
    s->listener=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    if(s->listener==BAD_SOCKET || !nonblocking(s->listener) ||
       bind(s->listener,(struct sockaddr*)&address,sizeof address)!=0 || listen(s->listener,1)!=0) {
        diag_server_destroy(s);return NULL;
    }
#ifdef _WIN32
    int length=sizeof address;
#else
    socklen_t length=sizeof address;
#endif
    if(getsockname(s->listener,(struct sockaddr*)&address,&length)!=0){diag_server_destroy(s);return NULL;}
    s->port=ntohs(address.sin_port);return s;
}
void diag_server_destroy(DiagServer* s) {
    if(!s)return;
    if(s->listener!=BAD_SOCKET)close_socket(s->listener);
    if(s->client!=BAD_SOCKET)close_socket(s->client);
    vb_stream_reset(&s->stream);
    Command* item=s->commands;
    while(item){Command* next=item->next;free(item->name);free(item);item=next;}
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}
uint16_t diag_server_port(const DiagServer* s){return s?s->port:0;}
bool diag_server_register(DiagServer* s,const char* name,DiagCommand cb,void* user) {
    if(!s || !name || !*name || !cb || !strcmp(name,"capabilities"))return false;
    for(const unsigned char* p=(const unsigned char*)name;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='_' || *p=='-'))return false;
    for(Command* p=s->commands;p;p=p->next)if(!strcmp(name,p->name))return false;
    Command* item=calloc(1,sizeof(*item));if(!item)return false;
    item->name=malloc(strlen(name)+1);if(!item->name){free(item);return false;}
    strcpy(item->name,name);item->callback=cb;item->user=user;item->next=s->commands;s->commands=item;return true;
}
bool diag_server_reply(DiagServer* s,const char* text) {
    if(!s || !text || s->client==BAD_SOCKET)return false;
    vb_stream_queue(&s->stream,text);return !s->stream.failed;
}
bool diag_server_protocol(DiagServer*s,size_t line,size_t output,DiagCommand error,void*user){
 if(!s||s->client!=BAD_SOCKET||!line||line>=sizeof s->stream.input||!output||output>64u*1024u*1024u)return false;
 s->line_limit=line;s->output_limit=output;s->framing_error=error;s->framing_user=user;return true;
}
static void framing_error(void*user,const char*reason){DiagServer*s=user;if(s->framing_error)s->framing_error(s->framing_user,s,reason);}
static void stream_open(DiagServer*s){
 vb_stream_reset(&s->stream);s->stream.line_limit=s->line_limit;s->stream.output_limit=s->output_limit;
 if(s->framing_error){s->stream.invalid=framing_error;s->stream.invalid_user=s;}
}
bool diag_server_raw_handler(DiagServer*s,DiagCommand cb,void*user){
 if(!s||!cb)return false;s->raw=cb;s->raw_user=user;return true;
}
bool diag_server_connected(const DiagServer*s){return s&&s->client!=BAD_SOCKET&&!s->disconnect;}
void diag_server_disconnect(DiagServer*s){if(s)s->disconnect=1;}
bool diag_server_write(DiagServer*s,const char*bytes,size_t n){
 if(!diag_server_connected(s)||(!bytes&&n))return false;
 vb_stream_queue_bytes(&s->stream,bytes,n);return !s->stream.failed;
}
static void dispatch(void* user,char* line) {
    DiagServer* s=user;
    if(s->disconnect)return;
    if(s->raw){s->raw(s->raw_user,s,line);return;}
    char* arguments=strchr(line,' ');
    if(arguments){*arguments++=0;while(*arguments==' ')++arguments;}else arguments=line+strlen(line);
    if(!strcmp(line,"capabilities")) {
        size_t size=32;for(Command* p=s->commands;p;p=p->next)size+=strlen(p->name)+3;
        char* reply=malloc(size);if(!reply){s->stream.failed=1;return;}
        strcpy(reply,"{\"commands\":[");
        for(Command* p=s->commands;p;p=p->next){strcat(reply,"\"");strcat(reply,p->name);strcat(reply,p->next?"\",":"\"");}
        strcat(reply,"]}");diag_server_reply(s,reply);free(reply);return;
    }
    for(Command* p=s->commands;p;p=p->next)if(!strcmp(line,p->name)){p->callback(p->user,s,arguments);return;}
    diag_server_reply(s,"{\"error\":\"unsupported command\"}");
}
bool diag_server_poll(DiagServer* s) {
    if(!s)return false;
    if(s->client==BAD_SOCKET) {
        sock_t client=accept(s->listener,NULL,NULL);
        if(client!=BAD_SOCKET) {
            if(!nonblocking(client)){close_socket(client);return false;}
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
            int no_sigpipe=1;
            if(setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe))!=0){close_socket(client);return false;}
#endif
            s->client=client;s->disconnect=0;stream_open(s);
        }else if(!vb_stream_would_block())return false;
    }
    if(s->client!=BAD_SOCKET && (s->disconnect || !vb_stream_poll(&s->stream,s->client,dispatch,s) || s->disconnect)) {
        close_socket(s->client);s->client=BAD_SOCKET;s->disconnect=0;vb_stream_reset(&s->stream);
    }
    return true;
}
