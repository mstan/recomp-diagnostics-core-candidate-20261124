#include "recomp_diagnostics/replay_c.h"
#include "replay_writer.h"
#include <cstdio>
#include <new>
#include <utility>
using recomp::diagnostics::ReplayWriter;
struct RecompReplayWriter {std::unique_ptr<ReplayWriter> writer;const char *failure=nullptr;};
static bool accept(RecompReplayWriter *w,bool ok) {
 if(w&&!ok&&!w->failure)w->failure=w->writer->error()?w->writer->error():"replay disk writer fell behind; bounded queue full";
 return ok;
}
template<class F> static bool call(RecompReplayWriter *w,F f) noexcept {
 if(!w||w->failure)return false;
 try{return accept(w,f());}catch(...){w->failure="replay writer allocation failed";return false;}
}
extern "C" RecompReplayWriter *recomp_replay_create(void) {
 try {auto writer=ReplayWriter::create(ReplayWriter::Format::Genesis32);if(!writer)return nullptr;return new RecompReplayWriter{std::move(writer),nullptr};}catch(...){return nullptr;}
}
extern "C" bool recomp_replay_begin(RecompReplayWriter*w,const char*p,const char*h,const char*c,const char*m,const char*e) {
 return call(w,[&]{if(!p||!h||!c||!m||!e){w->failure="invalid replay begin";return false;}
  return w->writer->begin(p,h,{{m,std::string(p)+".mods.toml"}},{{c,std::string(p)+".config.ini"},{e,std::string(p)+".mods.txt"}});});
}
extern "C" bool recomp_replay_snapshot(RecompReplayWriter*w,const void*p,size_t n){return call(w,[&]{return w->writer->snapshot(p,n,'K');});}
extern "C" bool recomp_replay_line(RecompReplayWriter*w,const char*s){return call(w,[&]{if(!s){w->failure="invalid replay line";return false;}return w->writer->line(s);});}
extern "C" bool recomp_replay_hashes(RecompReplayWriter*w,unsigned frame,const uint32_t h[4]) {
 return call(w,[&]{if(!h){w->failure="invalid replay hashes";return false;}char text[80];int n=std::snprintf(text,sizeof text,"H %u %08x %08x %08x %08x\n",frame,h[0],h[1],h[2],h[3]);return n>0&&n<(int)sizeof text&&w->writer->line(text);});
}
extern "C" bool recomp_replay_remove(RecompReplayWriter*w,const char*p){return call(w,[&]{if(!p){w->failure="invalid replay path";return false;}return w->writer->remove(p);});}
extern "C" bool recomp_replay_end(RecompReplayWriter*w){return call(w,[&]{return w->writer->end();});}
extern "C" const char *recomp_replay_error(RecompReplayWriter*w){return w?(w->failure?w->failure:w->writer->error()):nullptr;}
extern "C" const char *recomp_replay_destroy(RecompReplayWriter*w){if(!w)return nullptr;const char *result=w->writer->shutdown();if(w->failure)result=w->failure;delete w;return result;}

