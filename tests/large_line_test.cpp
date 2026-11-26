#include "diagnostics_server.h"
#include <atomic>
#include <thread>
#include <string>
#include <cstdlib>
#include <algorithm>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket=SOCKET;
#define CLOSE closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using Socket=int;
#define CLOSE close
#endif
#define CHECK(x) do{if(!(x))std::abort();}while(0)
static void echo(void*,DiagServer*s,const char*p){CHECK(diag_server_reply(s,p));}
static void error(void*,DiagServer*s,const char*){CHECK(diag_server_reply(s,"framing_error"));}
static void send_all(Socket fd,const std::string&s){size_t n=0;while(n<s.size()){int k=send(fd,s.data()+n,int(std::min<size_t>(997,s.size()-n)),0);CHECK(k>0);n+=k;}}
static std::string line(Socket fd){std::string s;for(;;){char c;CHECK(recv(fd,&c,1,0)==1);if(c=='\n')return s;s+=c;}}
static void profile(size_t limit){
    DiagServer*s=diag_server_create(0);CHECK(s);
    CHECK(!diag_server_protocol(s,16u*1024u*1024u+1,64u*1024u*1024u,error,nullptr));
    CHECK(diag_server_protocol(s,limit,64u*1024u*1024u,error,nullptr));CHECK(diag_server_raw_handler(s,echo,nullptr));
    std::atomic<bool>done=false;
    std::thread client([&]{Socket fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(diag_server_port(s));CHECK(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)==0);
        std::string exact(limit,'x');send_all(fd,exact+"\n");CHECK(line(fd)==exact);
        send_all(fd,std::string(limit+1,'y')+"\nsmall\n");CHECK(line(fd)=="framing_error");CHECK(line(fd)=="small");
        send_all(fd,std::string("bad\0line\nok\r\n",13));CHECK(line(fd)=="framing_error");CHECK(line(fd)=="ok");
        CLOSE(fd);done=true;
    });
    while(!done){CHECK(diag_server_poll(s));std::this_thread::yield();}client.join();diag_server_destroy(s);
}
int main(){profile(8191);profile(65536);}
