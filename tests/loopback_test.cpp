#include "diagnostics_server.h"
#include <atomic>
#include <thread>
#include <string>
#include <cstdlib>
#include <cstring>
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
struct State {std::thread::id owner;unsigned calls=0;};
static void echo(void* user,DiagServer*s,const char* args){auto& state=*static_cast<State*>(user);CHECK(state.owner==std::this_thread::get_id());++state.calls;CHECK(diag_server_reply(s,args));}
static Socket connect_to(uint16_t port){Socket fd=socket(AF_INET,SOCK_STREAM,0);sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=htons(port);CHECK(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)==0);return fd;}
static void send_all(Socket fd,const std::string& text){size_t sent=0;while(sent<text.size()){int n=send(fd,text.data()+sent,int(text.size()-sent),0);CHECK(n>0);sent+=n;}}
static std::string line(Socket fd){std::string out;for(;;){char c;CHECK(recv(fd,&c,1,0)==1);if(c==10)return out;out+=c;}}
int main(){DiagServer*s=diag_server_create(0);CHECK(s && diag_server_port(s));State state{std::this_thread::get_id()};CHECK(diag_server_register(s,"echo",echo,&state));CHECK(!diag_server_register(s,"echo",echo,&state));CHECK(!diag_server_register(s,"bad name",echo,&state));
 std::atomic<bool>done=false;
 std::thread client([&]{Socket fd=connect_to(diag_server_port(s));send_all(fd,"ec");send_all(fd,std::string("ho first")+char(13)+char(10)+"echo second"+char(10)+"capabilities"+char(10)+"missing"+char(10));CHECK(line(fd)=="first");CHECK(line(fd)=="second");CHECK(line(fd).find("echo")!=std::string::npos);CHECK(line(fd).find("unsupported")!=std::string::npos);CLOSE(fd);done=true;});
 while(!done){CHECK(diag_server_poll(s));std::this_thread::yield();}client.join();CHECK(state.calls==2);
 // Service disconnect and prove a clean connection can register input again.
 CHECK(diag_server_poll(s));done=false;
 std::thread second([&]{Socket fd=connect_to(diag_server_port(s));send_all(fd,std::string("echo reconnect")+char(10));CHECK(line(fd)=="reconnect");CLOSE(fd);done=true;});
 while(!done){CHECK(diag_server_poll(s));std::this_thread::yield();}second.join();CHECK(state.calls==3);diag_server_destroy(s);
 // Four concurrent tools retain independent partial lines and response queues.
 s=diag_server_create(0);CHECK(s);state.calls=0;
 CHECK(!diag_server_client_limit(s,0) && !diag_server_client_limit(s,5));
 CHECK(diag_server_client_limit(s,4));CHECK(diag_server_register(s,"echo",echo,&state));done=false;
 std::thread parallel([&]{
  Socket clients[4];for(unsigned i=0;i<4;++i)clients[i]=connect_to(diag_server_port(s));
  for(unsigned i=0;i<4;++i)send_all(clients[i],"echo client");
  for(unsigned i=0;i<4;++i)send_all(clients[i],std::to_string(i)+"\n");
  for(unsigned i=0;i<4;++i)CHECK(line(clients[i])=="client"+std::to_string(i));
  // An oversized line disconnects only that client, without poisoning peers.
  send_all(clients[0],std::string(8192,'x')+"\n");char byte;
  CHECK(recv(clients[0],&byte,1,0)<=0);CLOSE(clients[0]);
  for(unsigned i=1;i<4;++i){send_all(clients[i],"echo retained\n");CHECK(line(clients[i])=="retained");}
  clients[0]=connect_to(diag_server_port(s));send_all(clients[0],"echo replacement\n");
  CHECK(line(clients[0])=="replacement");
  for(auto client:clients)CLOSE(client);done=true;
 });
 while(!done){CHECK(diag_server_poll(s));std::this_thread::yield();}
 parallel.join();CHECK(state.calls==8);diag_server_destroy(s);return 0;
}
