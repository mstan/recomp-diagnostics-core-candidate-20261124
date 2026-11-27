/* SPDX-License-Identifier: MIT
 * Derived from the qualified SNES diagnostic byte writer. The byte-ring
 * profile preserves producer admission and file/lifecycle observations. */
#include <recomp_diagnostics/trace_writer.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <memory>
#include <string>
#include <thread>
#ifdef _WIN32
#include <direct.h>
#define trace_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define trace_mkdir(p) mkdir(p,0755)
#endif

enum { TraceOk, TraceFull, TraceRow, TraceIo };
struct RecompTraceWriter {
    RecompTraceOptions options;
    std::string stem, header;
    std::unique_ptr<unsigned char[]> ring, row;
    std::unique_ptr<char[]> file_buffer;
    uint32_t capacity, mask, head=0;
    std::atomic<uint32_t> read_pos{0}, write_pos{0};
    std::atomic<int> stopped{0}, error{TraceOk};
    std::thread worker;
    RecompTraceWriter(const char* s,const char* h,RecompTraceOptions o)
        : options(o),stem(s),header(h),ring(new unsigned char[o.queue_bytes]),
          row(new unsigned char[o.row_bytes]),file_buffer(new char[o.buffer_bytes]),
          capacity(static_cast<uint32_t>(o.queue_bytes)),mask(capacity-1) {}
    void fail(int reason) { int expected=TraceOk;error.compare_exchange_strong(expected,reason); }
    uint32_t ticks() const {
        if(options.host.ticks_ms)return options.host.ticks_ms(options.host.user);
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep() const {
        if(options.host.sleep_ms)options.host.sleep_ms(options.host.user,options.poll_ms);
        else std::this_thread::sleep_for(std::chrono::milliseconds(options.poll_ms));
    }
    void put(uint32_t at,const void* source,size_t n) {
        size_t first=capacity-at;if(first>n)first=n;
        std::memcpy(ring.get()+at,source,first);
        std::memcpy(ring.get(),static_cast<const unsigned char*>(source)+first,n-first);
    }
    void get(uint32_t at,void* destination,size_t n) const {
        size_t first=capacity-at;if(first>n)first=n;
        std::memcpy(destination,ring.get()+at,first);
        std::memcpy(static_cast<unsigned char*>(destination)+first,ring.get(),n-first);
    }
    FILE* segment(unsigned index,size_t& bytes) {
        char path[2064];
        if(index>options.keep_segments) {
            std::snprintf(path,sizeof(path),"%s-%03u.csv",stem.c_str(),index-options.keep_segments);
            if(std::remove(path)&&errno!=ENOENT)return nullptr;
        }
        std::snprintf(path,sizeof(path),"%s-%03u.csv",stem.c_str(),index);
        FILE* file=std::fopen(path,"wb");if(!file)return nullptr;
        if(std::setvbuf(file,file_buffer.get(),_IOFBF,options.buffer_bytes)) { std::fclose(file);return nullptr; }
        bytes=header.size();
        if(std::fwrite(header.data(),1,bytes,file)!=bytes) { std::fclose(file);return nullptr; }
        return file;
    }
    void run() noexcept {
        FILE* file=nullptr;
        try {
            const auto slash=stem.find_last_of("/\\");
            if(slash!=std::string::npos)trace_mkdir(stem.substr(0,slash).c_str());
            size_t bytes=0;unsigned index=1;uint32_t tail=0;
            file=segment(index,bytes);if(!file){fail(TraceIo);return;}
            uint32_t last_flush=ticks();bool dirty=true;
            if(options.host.worker_ready)options.host.worker_ready(options.host.user);
            for(;;) {
                if(write_pos.load()!=tail) {
                    uint32_t length=0;get(tail,&length,sizeof(length));
                    if(!length||length>options.row_bytes){fail(TraceRow);break;}
                    get((tail+sizeof(length))&mask,row.get(),length);
                    tail=(tail+sizeof(length)+length)&mask;
                    read_pos.store(tail); // worker owns its row before slow I/O
                    if(bytes>=options.segment_bytes) {
                        if(std::fclose(file)){file=nullptr;fail(TraceIo);break;}
                        file=nullptr;file=segment(++index,bytes);
                        if(!file){fail(TraceIo);break;}
                    }
                    if(std::fwrite(row.get(),1,length,file)!=length){fail(TraceIo);break;}
                    bytes+=length;dirty=true;
                } else {
                    if(stopped.load()||error.load()) {
                        if(write_pos.load()==tail)break;
                        continue;
                    }
                    sleep();
                }
                const uint32_t now=ticks();
                if(dirty&&static_cast<uint32_t>(now-last_flush)>=options.flush_ms) {
                    if(std::fflush(file)){fail(TraceIo);break;}
                    dirty=false;last_flush=now;
                }
            }
        } catch(...) { fail(TraceIo); }
        if(file&&std::fclose(file))fail(TraceIo);
    }
};
extern "C" RecompTraceOptions recomp_trace_options(size_t bytes,unsigned keep) {
    RecompTraceOptions o{};o.size=sizeof(o);o.queue_bytes=4u*1024u*1024u;
    o.row_bytes=8192;o.buffer_bytes=65536;o.segment_bytes=bytes;
    o.keep_segments=keep;o.poll_ms=2;o.flush_ms=1000;return o;
}
extern "C" RecompTraceWriter* recomp_trace_open(const char* stem,const char* header,const RecompTraceOptions* options) {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    (void)stem;(void)header;(void)options;return nullptr;
#else
    if(!stem||!header||!options||options->size!=sizeof(*options)||std::strlen(stem)>=2048)return nullptr;
    const auto& o=*options;
    if(!o.segment_bytes||!o.keep_segments||o.queue_bytes<8||o.queue_bytes>(1u<<30)||
        (o.queue_bytes&(o.queue_bytes-1))||!o.row_bytes||o.row_bytes>o.queue_bytes-5||
        !o.buffer_bytes||o.buffer_bytes>1024u*1024u)return nullptr;
    try {
        auto w=std::make_unique<RecompTraceWriter>(stem,header,o);
        auto* raw=w.get();w->worker=std::thread([raw]{raw->run();});return w.release();
    } catch(...) { return nullptr; }
#endif
}
extern "C" size_t recomp_trace_free_bytes(const RecompTraceWriter* w) {
    return w?((w->read_pos.load()+w->capacity-w->head-1)&w->mask):0;
}
extern "C" int recomp_trace_write(RecompTraceWriter* w,const void* data,size_t n) {
    if(!w||w->error.load()||w->stopped.load())return 0;
    if(!data||!n||n>w->options.row_bytes){w->fail(TraceRow);return 0;}
    if(n+sizeof(uint32_t)>recomp_trace_free_bytes(w)){w->fail(TraceFull);return 0;}
    const auto length=static_cast<uint32_t>(n);
    w->put(w->head,&length,sizeof(length));w->put((w->head+sizeof(length))&w->mask,data,n);
    w->head=(w->head+sizeof(length)+length)&w->mask;w->write_pos.store(w->head);return 1;
}
extern "C" const char* recomp_trace_error(const RecompTraceWriter* w) {
    if(!w)return nullptr;
    switch(w->error.load()) {
        case TraceFull:return "disk writer fell behind; bounded queue full";
        case TraceRow:return "diagnostic row exceeds its bounded buffer";
        case TraceIo:return "cannot write diagnostic file";
        default:return nullptr;
    }
}
extern "C" const char* recomp_trace_close(RecompTraceWriter* w) {
    if(!w)return nullptr;w->stopped.store(1);
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    if(w->worker.joinable())w->worker.join();
#endif
    const char* error=recomp_trace_error(w);delete w;return error;
}
