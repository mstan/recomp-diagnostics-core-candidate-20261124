#include "replay_writer.h"
#include <filesystem>
#include <chrono>
#include <thread>
#include <limits>
#include <cstdlib>
#include <vector>
#define CHECK(x) do{if(!(x))std::abort();}while(0)
using namespace recomp::diagnostics;
static void delay(const char* value){
#ifdef _WIN32
 _putenv_s("RECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US",value);
#else
 setenv("RECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US",value,1);
#endif
}
int main(){
 auto root=std::filesystem::current_path()/("replay-bounds-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directory(root);
 {auto w=ReplayWriter::create();CHECK(w && !w->line("before begin") && w->status()==ReplayWriter::Status::Invalid);CHECK(w->shutdown());}
 {auto w=ReplayWriter::create();CHECK(w->begin((root/"overflow").string(),"fixture",{},{}));char byte=0;CHECK(!w->snapshot(&byte,std::numeric_limits<std::size_t>::max(),'C'));CHECK(w->status()==ReplayWriter::Status::Invalid);CHECK(w->shutdown());}
 {auto w=ReplayWriter::create();CHECK(w->begin((root/"nullparts").string(),"fixture",{},{}));CHECK(!w->frame(0,nullptr,1) && w->status()==ReplayWriter::Status::Invalid);CHECK(w->shutdown());}
 {auto w=ReplayWriter::create();CHECK(w->begin((root/"partoverflow").string(),"fixture",{},{}));char byte=0;ReplayWriter::Part p[2]={{&byte,std::numeric_limits<std::size_t>::max(),false},{&byte,16,false}};CHECK(!w->frame(0,p,2) && w->status()==ReplayWriter::Status::Invalid);CHECK(w->shutdown());}
 {auto w=ReplayWriter::create();CHECK(w->begin((root/"ended").string(),"fixture",{},{}));CHECK(w->end());CHECK(!w->snapshot("x",1,'C') && w->status()==ReplayWriter::Status::Invalid);CHECK(w->shutdown());}
 delay("2000000");
 {auto w=ReplayWriter::create();CHECK(w->begin((root/"recovery").string(),"fixture",{},{}));CHECK(w->frame(0,nullptr,0));std::vector<unsigned char> payload(24u*1024u*1024u,0x55);CHECK(w->snapshot(payload.data(),payload.size(),'C'));CHECK(!w->snapshot(payload.data(),16u*1024u*1024u,'C'));CHECK(w->status()==ReplayWriter::Status::Full);
 while(w->status()==ReplayWriter::Status::Full)std::this_thread::yield();
 CHECK(w->status()==ReplayWriter::Status::Ok);CHECK(w->end());CHECK(!w->shutdown());}
 delay("");std::filesystem::remove_all(root);return 0;
}
