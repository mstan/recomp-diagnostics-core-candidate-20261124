/* SPDX-License-Identifier: MIT */
#include <recomp_diagnostics/trace_writer.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"trace line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace fs=std::filesystem;
struct Gate {std::atomic<bool> entered{false},released{false};};
struct Clock {std::atomic<uint32_t> ticks{0xfffffff0u};};
static uint32_t ticks(void* p) {return static_cast<Clock*>(p)->ticks.load();}
static void advance(void* p,unsigned ms) {static_cast<Clock*>(p)->ticks.fetch_add(ms);std::this_thread::yield();}
static void ready(void* p) {auto* g=static_cast<Gate*>(p);g->entered=true;while(!g->released)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
static void entered(Gate& g,RecompTraceWriter* w) {while(!g.entered&&!recomp_trace_error(w))std::this_thread::sleep_for(std::chrono::milliseconds(1));CHECK(g.entered);}
static std::string text(const fs::path& p) {std::ifstream f(p,std::ios::binary);CHECK(f);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
int main() {
    const auto token=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root=fs::temp_directory_path()/("core-trace-"+std::to_string(token));fs::create_directories(root);
    auto config=recomp_trace_options(64,2);CHECK(config.queue_bytes==4194304&&config.row_bytes==8192);
    config.queue_bytes=256;config.row_bytes=32;
    auto bad=config;bad.queue_bytes=255;CHECK(!recomp_trace_open("bad","h\n",&bad));
    Gate gate;config.host={&gate,nullptr,nullptr,ready};
    auto* w=recomp_trace_open((root/"bounded").string().c_str(),"h\n",&config);CHECK(w);entered(gate,w);
    const char row[]="123456789012345\n";unsigned accepted=0;
    while(recomp_trace_write(w,row,16))++accepted;
    CHECK(accepted==12&&recomp_trace_free_bytes(w)==15);
    CHECK(std::strstr(recomp_trace_error(w),"queue full"));
    CHECK(!recomp_trace_write(w,nullptr,0));CHECK(std::strstr(recomp_trace_error(w),"queue full"));
    gate.released=true;CHECK(std::strstr(recomp_trace_close(w),"queue full"));
    // Header2 +4rows64 =>66, rotate before row5; keep2 retires first file.
    CHECK(!fs::exists(root/"bounded-001.csv"));
    CHECK(text(root/"bounded-002.csv")==std::string("h\n")+row+row+row+row);
    CHECK(text(root/"bounded-003.csv")==std::string("h\n")+row+row+row+row);
    Gate invalid;config.segment_bytes=1024;config.host={&invalid,nullptr,nullptr,ready};
    w=recomp_trace_open((root/"invalid").string().c_str(),"h\n",&config);CHECK(w);entered(invalid,w);
    CHECK(recomp_trace_write(w,"good\n",5));CHECK(!recomp_trace_write(w,row,33));
    invalid.released=true;CHECK(std::strstr(recomp_trace_close(w),"bounded buffer"));
    CHECK(text(root/"invalid-001.csv")=="h\ngood\n");
    config.host={};config.segment_bytes=1024*1024;
    w=recomp_trace_open((root/"wrap").string().c_str(),"h\n",&config);CHECK(w);
    std::string expected="h\n";
    for(unsigned i=0;i<4096;++i) {
        const std::string line=std::to_string(i)+"\n";
        while(recomp_trace_free_bytes(w)<line.size()+4&&!recomp_trace_error(w))std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(recomp_trace_write(w,line.data(),line.size()));expected+=line;
    }
    CHECK(!recomp_trace_close(w));CHECK(text(root/"wrap-001.csv")==expected);
    // Idle periodic flush must survive the native 32-bit tick wrap.
    Clock clock;config.host={&clock,ticks,advance,nullptr};
    w=recomp_trace_open((root/"flush").string().c_str(),"h\n",&config);CHECK(w);
    CHECK(recomp_trace_write(w,"visible\n",8));
    for(;;) {
        std::ifstream f(root/"flush-001.csv",std::ios::binary);
        const std::string current{std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
        if(current=="h\nvisible\n")break;
        CHECK(!recomp_trace_error(w));std::this_thread::yield();
    }
    CHECK(clock.ticks.load()<0xfffffff0u);CHECK(!recomp_trace_close(w));config.host={};
    w=recomp_trace_open((root/"absent/parent/trace").string().c_str(),"h\n",&config);CHECK(w);
    while(!recomp_trace_error(w))std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(!recomp_trace_write(w,"x\n",2));CHECK(recomp_trace_close(w));
    CHECK(!recomp_trace_close(nullptr)&&!recomp_trace_error(nullptr));
    fs::remove_all(root);
    std::puts("trace profile exact byte admission, first-error/prefix drain, row bound, rotation, wrap, idle flush/tick wrap and I/O failure PASS");
}
