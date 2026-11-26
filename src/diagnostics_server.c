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
typedef struct Peer {
    sock_t socket;
    DiagStream stream;
    int disconnect;
} Peer;
struct DiagServer {
    sock_t listener;
    uint16_t port;
    Peer peers[4];
    unsigned client_limit;
    Peer* active;
    Command* commands;
    DiagCommand raw;void* raw_user;
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
    for(unsigned i=0;i<4;++i)s->peers[i].socket=BAD_SOCKET;
    s->client_limit=1;
    s->listener=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in address={0}; address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(port);
    if(s->listener==BAD_SOCKET || !nonblocking(s->listener) ||
       bind(s->listener,(struct sockaddr*)&address,sizeof address)!=0 || listen(s->listener,4)!=0) {
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
    for(unsigned i=0;i<4;++i) {
        if(s->peers[i].socket!=BAD_SOCKET)close_socket(s->peers[i].socket);
        vb_stream_reset(&s->peers[i].stream);
    }
    Command* item=s->commands;
    while(item){Command* next=item->next;free(item->name);free(item);item=next;}
    free(s);
#ifdef _WIN32
    WSACleanup();
#endif
}
uint16_t diag_server_port(const DiagServer* s){return s?s->port:0;}
static Peer* reply_peer(DiagServer* s) {
    if(!s)return NULL;
    if(s->active)return s->active;
    for(unsigned i=0;i<s->client_limit;++i)
        if(s->peers[i].socket!=BAD_SOCKET)return s->peers+i;
    return NULL;
}
bool diag_server_client_limit(DiagServer* s,unsigned clients) {
    if(!s || !clients || clients>4 || diag_server_connected(s))return false;
    s->client_limit=clients;return true;
}
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
    Peer* p=reply_peer(s);
    if(!p || !text || p->disconnect)return false;
    vb_stream_queue(&p->stream,text);return !p->stream.failed;
}
bool diag_server_protocol(DiagServer*s,size_t line,size_t output,DiagCommand error,void*user){
 if(!s||diag_server_connected(s)||!line||line>16u*1024u*1024u||!output||output>64u*1024u*1024u)return false;
 s->line_limit=line;s->output_limit=output;s->framing_error=error;s->framing_user=user;return true;
}
static void framing_error(void*user,const char*reason){DiagServer*s=user;if(s->framing_error)s->framing_error(s->framing_user,s,reason);}
static void stream_open(DiagServer*s,Peer*p){
 vb_stream_reset(&p->stream);p->stream.line_limit=s->line_limit;p->stream.output_limit=s->output_limit;
 if(s->framing_error){p->stream.invalid=framing_error;p->stream.invalid_user=s;}
}
bool diag_server_raw_handler(DiagServer*s,DiagCommand cb,void*user){
 if(!s||!cb)return false;s->raw=cb;s->raw_user=user;return true;
}
bool diag_server_connected(const DiagServer*s){
 if(!s)return false;
 if(s->active)return s->active->socket!=BAD_SOCKET&&!s->active->disconnect;
 for(unsigned i=0;i<s->client_limit;++i)
  if(s->peers[i].socket!=BAD_SOCKET&&!s->peers[i].disconnect)return true;
 return false;
}
void diag_server_disconnect(DiagServer*s){Peer*p=reply_peer(s);if(p)p->disconnect=1;}
size_t diag_server_pending_output(const DiagServer*s){
 size_t pending=0;if(s)for(unsigned i=0;i<s->client_limit;++i)
  pending+=s->peers[i].stream.size-s->peers[i].stream.sent;
 return pending;
}
bool diag_server_write(DiagServer*s,const char*bytes,size_t n){
 Peer*p=reply_peer(s);if(!p||p->disconnect||(!bytes&&n))return false;
 vb_stream_queue_bytes(&p->stream,bytes,n);return !p->stream.failed;
}
static void dispatch(void* user,char* line) {
    DiagServer* s=user;
    if(!s->active || s->active->disconnect)return;
    if(s->raw){s->raw(s->raw_user,s,line);return;}
    char* arguments=strchr(line,' ');
    if(arguments){*arguments++=0;while(*arguments==' ')++arguments;}else arguments=line+strlen(line);
    if(!strcmp(line,"capabilities")) {
        size_t size=32;for(Command* p=s->commands;p;p=p->next)size+=strlen(p->name)+3;
        char* reply=malloc(size);if(!reply){s->active->stream.failed=1;return;}
        strcpy(reply,"{\"commands\":[");
        for(Command* p=s->commands;p;p=p->next){strcat(reply,"\"");strcat(reply,p->name);strcat(reply,p->next?"\",":"\"");}
        strcat(reply,"]}");diag_server_reply(s,reply);free(reply);return;
    }
    for(Command* p=s->commands;p;p=p->next)if(!strcmp(line,p->name)){p->callback(p->user,s,arguments);return;}
    diag_server_reply(s,"{\"error\":\"unsupported command\"}");
}
bool diag_server_poll(DiagServer* s) {
    if(!s || s->active)return false;
    for(unsigned i=0;i<s->client_limit;++i) {
        Peer* p=s->peers+i;
        if(p->socket!=BAD_SOCKET)continue;
        sock_t client=accept(s->listener,NULL,NULL);
        if(client!=BAD_SOCKET) {
            if(!nonblocking(client)){close_socket(client);return false;}
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
            int no_sigpipe=1;
            if(setsockopt(client,SOL_SOCKET,SO_NOSIGPIPE,&no_sigpipe,sizeof(no_sigpipe))!=0){close_socket(client);return false;}
#endif
            p->socket=client;p->disconnect=0;stream_open(s,p);
        }else {
            if(!vb_stream_would_block())return false;
            break;
        }
    }
    for(unsigned i=0;i<s->client_limit;++i) {
        Peer* p=s->peers+i;
        if(p->socket==BAD_SOCKET)continue;
        s->active=p;
        if(p->disconnect || !vb_stream_poll(&p->stream,p->socket,dispatch,s) || p->disconnect) {
            close_socket(p->socket);p->socket=BAD_SOCKET;p->disconnect=0;vb_stream_reset(&p->stream);
        }
        s->active=NULL;
    }
    return true;
}
