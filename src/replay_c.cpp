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
extern "C" RecompReplayWriter *recomp_replay_create_profile(RecompReplayFormat format,size_t capacity) {
 try {
  ReplayWriter::Format selected;
  switch(format) {
   case RECOMP_REPLAY_HASHED64:selected=ReplayWriter::Format::Hashed64;break;
   case RECOMP_REPLAY_GENESIS32:selected=ReplayWriter::Format::Genesis32;break;
   case RECOMP_REPLAY_SNES32:selected=ReplayWriter::Format::Snes32;break;
   default:return nullptr;
  }
  auto writer=ReplayWriter::create_with_capacity(selected,capacity);
  if(!writer)return nullptr;
  return new RecompReplayWriter{std::move(writer),nullptr};
 }catch(...){return nullptr;}
}
extern "C" bool recomp_replay_begin(RecompReplayWriter*w,const char*p,const char*h,const char*c,const char*m,const char*e) {
 return call(w,[&]{if(!p||!h||!c||!m||!e){w->failure="invalid replay begin";return false;}
  return w->writer->begin(p,h,{{m,std::string(p)+".mods.toml"}},{{c,std::string(p)+".config.ini"},{e,std::string(p)+".mods.txt"}});});
}
extern "C" bool recomp_replay_snapshot(RecompReplayWriter*w,const void*p,size_t n){return call(w,[&]{return w->writer->snapshot(p,n,'K');});}
extern "C" bool recomp_replay_snapshot_raw(RecompReplayWriter*w,const void*p,size_t n){return call(w,[&]{return w->writer->snapshot_raw(p,n);});}
extern "C" bool recomp_replay_begin_with_sidecars(RecompReplayWriter*w,const char*p,const char*h,
 const RecompReplaySidecar*copies,size_t nc,const RecompReplaySidecar*texts,size_t nt) {
 return call(w,[&]{
  if(!p||!h||nc>64||nt>64||(nc&&!copies)||(nt&&!texts)) {
   w->failure="invalid replay begin";return false;
  }
  std::vector<ReplayWriter::Sidecar> copy,inline_text;
  for(size_t i=0;i<nc;++i) {
   if(!copies[i].source||!copies[i].dest){w->failure="invalid replay sidecar";return false;}
   copy.push_back({copies[i].source,copies[i].dest});
  }
  for(size_t i=0;i<nt;++i) {
   if(!texts[i].source||!texts[i].dest){w->failure="invalid replay inline text";return false;}
   inline_text.push_back({texts[i].source,texts[i].dest});
  }
  return w->writer->begin(p,h,copy,inline_text);
 });
}
extern "C" bool recomp_replay_begin_files(RecompReplayWriter*w,const char*p,const char*h,
 const char*config,const char*mods,const char*effective) {
 return call(w,[&]{
  if(!p||!h||!config||!mods||!effective){w->failure="invalid replay begin";return false;}
  return w->writer->begin(p,h,{{config,std::string(p)+".config.ini"},
                              {mods,std::string(p)+".mods.toml"}},
                             {{effective,std::string(p)+".mods.txt"}});
 });
}
extern "C" bool recomp_replay_frame_parts(RecompReplayWriter*w,uint64_t frame,
 const RecompReplayPart*parts,size_t count) {
 return call(w,[&]{
  if(count>ReplayWriter::kMaxParts||(count&&!parts)){w->failure="invalid replay frame parts";return false;}
  ReplayWriter::Part native[ReplayWriter::kMaxParts]{};
  for(size_t i=0;i<count;++i)native[i]={parts[i].data,parts[i].size,parts[i].reuse};
  return w->writer->frame(frame,native,static_cast<unsigned>(count));
 });
}
extern "C" bool recomp_replay_line(RecompReplayWriter*w,const char*s){return call(w,[&]{if(!s){w->failure="invalid replay line";return false;}return w->writer->line(s);});}
extern "C" bool recomp_replay_hashes(RecompReplayWriter*w,unsigned frame,const uint32_t h[4]) {
 return call(w,[&]{if(!h){w->failure="invalid replay hashes";return false;}char text[80];int n=std::snprintf(text,sizeof text,"H %u %08x %08x %08x %08x\n",frame,h[0],h[1],h[2],h[3]);return n>0&&n<(int)sizeof text&&w->writer->line(text);});
}
extern "C" bool recomp_replay_remove(RecompReplayWriter*w,const char*p){return call(w,[&]{if(!p){w->failure="invalid replay path";return false;}return w->writer->remove(p);});}
extern "C" bool recomp_replay_end(RecompReplayWriter*w){return call(w,[&]{return w->writer->end();});}
extern "C" const char *recomp_replay_error(RecompReplayWriter*w){return w?(w->failure?w->failure:w->writer->error()):nullptr;}
extern "C" const char *recomp_replay_destroy(RecompReplayWriter*w){if(!w)return nullptr;const char *result=w->writer->shutdown();if(w->failure)result=w->failure;delete w;return result;}

