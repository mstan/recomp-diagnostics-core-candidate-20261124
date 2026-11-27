/* Bounded native-input recording. The producer copies owned state; the
 * shared worker owns queueing, hashing, formatting, flush and shutdown.
 * Policy owns game record shapes and mode serialization. C++17. */
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace recomp::diagnostics {
template<size_t Capacity,class Policy> class NativeInputWriter {
    using Row=typename Policy::Row;
    using State=typename Policy::State;
    using Mode=typename Policy::Mode;
    enum class Kind { Native, Controller, StateLoad, Development, Mode, Flush };
    enum Error { None, Full, Io, Start, Invalid };
    struct Event {
        Row row{};
        State state{};
        unsigned size=0;
        Kind kind=Kind::Native;
    };
    static_assert(Capacity>1,"queue must hold at least one event");
    std::unique_ptr<Event[]> queue;
    std::ofstream file;
    std::ofstream modes;
    bool modes_enabled=false;
    std::thread thread;
    std::atomic<size_t> read{0},write{0};
    std::atomic<unsigned> flushes{0};
    unsigned requested_flushes=0;
    std::atomic<int> error{None};
    std::atomic<bool> stop{false};
    bool opened=false;
#ifdef RECOMP_DIAGNOSTIC_NATIVE_INPUT_WRITER_TEST
    void (*gate)()=nullptr;
#endif
    void fail(Error value) {int expected=None;error.compare_exchange_strong(expected,value);}
    Event *reserve() {
        if(!opened || error.load(std::memory_order_relaxed))return nullptr;
        size_t head=write.load(std::memory_order_relaxed);
        if((head+1)%Capacity==read.load(std::memory_order_acquire)) {fail(Full);return nullptr;}
        return &queue[head];
    }
    void publish() {
        write.store((write.load(std::memory_order_relaxed)+1)%Capacity,std::memory_order_release);
    }
    void run() {
#ifdef RECOMP_DIAGNOSTIC_NATIVE_INPUT_WRITER_TEST
        if(gate)gate();
#endif
        auto last_flush=std::chrono::steady_clock::now();
        bool dirty=false;
        for(;;) {
            size_t tail=read.load(std::memory_order_relaxed);
            if(tail!=write.load(std::memory_order_acquire)) {
                const auto& event=queue[tail];
                switch(event.kind) {
                case Kind::Native:
                    file<<"N ";
                    for(unsigned i=0;i<event.row.size();++i)
                        file<<(i?",":"")<<(i==Policy::HashColumn?int64_t(Policy::hash(event.state,event.size)):event.row[i]);
                    file<<'\n';break;
                case Kind::Controller:
                    file<<"C "<<event.row[0]<<' '<<event.row[1]<<' '<<event.row[2]<<'\n';break;
                case Kind::StateLoad:file<<"# network state load\n";break;
                case Kind::Development:
                    file<<"D "<<event.row[0]<<' '<<event.row[1]<<' '<<event.row[2]
                        <<' '<<event.row[3]<<' '<<event.row[4]<<'\n';break;
                case Kind::Mode: {
                    Policy::write_mode(modes,event.row);
                    break;
                }
                case Kind::Flush:
                    file.flush();if(modes_enabled)modes.flush();
                    flushes.fetch_add(1,std::memory_order_release);break;
                }
                // Do not release a slot until hashing/formatting has finished.
                read.store((tail+1)%Capacity,std::memory_order_release);
                dirty=true;
                if(!file || (modes_enabled && !modes)) {fail(Io);break;}
            }else {
                if(stop.load(std::memory_order_acquire) || error.load())break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            auto now=std::chrono::steady_clock::now();
            // Wall time also flushes a hung/paused simulation's final input.
            if(dirty && now-last_flush>=std::chrono::milliseconds(100)) {
                file.flush();if(modes_enabled)modes.flush();last_flush=now;dirty=false;
                if(!file || (modes_enabled && !modes)) {fail(Io);break;}
            }
        }
        file.close();if(!file)fail(Io);
        if(modes_enabled) {modes.close();if(!modes)fail(Io);}
    }
public:
    NativeInputWriter()=default;
    NativeInputWriter(const NativeInputWriter&)=delete;
    NativeInputWriter& operator=(const NativeInputWriter&)=delete;
    ~NativeInputWriter() {close();}
#ifdef RECOMP_DIAGNOSTIC_NATIVE_INPUT_WRITER_TEST
    void test_gate(void (*callback)()) {gate=callback;}
#endif
    void open(const std::filesystem::path& path,const std::string& header,
              const std::filesystem::path& mode_path={}) {
        close();clear();
        // Session setup is outside steady-state recording. Make the header
        // observable immediately and report path/permissions errors now.
        file.open(path,std::ios::out|std::ios::trunc);
        file<<header;file.flush();
        if(!file) {fail(Io);file.close();return;}
        if(!mode_path.empty()) {
            modes.open(mode_path,std::ios::out|std::ios::trunc);
            modes<<Policy::mode_header();
            modes.flush();
            if(!modes) {fail(Io);modes.close();file.close();return;}
            modes_enabled=true;
        }
        try {
            queue=std::make_unique<Event[]>(Capacity);
            opened=true;thread=std::thread([this] {run();});
        }catch(...) {opened=false;fail(Start);file.close();if(modes_enabled)modes.close();queue.reset();}
    }
    bool native(const Row& row,const State& state,unsigned size) {
        if(size>state.size()) {fail(Invalid);return false;}
        auto *event=reserve();if(!event)return false;
        event->kind=Kind::Native;event->row=row;event->state=state;event->size=size;
        publish();return true;
    }
    bool controller(unsigned sequence,unsigned seat,uint16_t buttons) {
        auto *event=reserve();if(!event)return false;
        event->kind=Kind::Controller;event->row[0]=sequence;event->row[1]=seat;event->row[2]=buttons;
        publish();return true;
    }
    void state_load() {auto *event=reserve();if(event) {event->kind=Kind::StateLoad;publish();}}
    void development(unsigned sequence,unsigned command,unsigned seat,unsigned hp,unsigned ammo) {
        auto *event=reserve();if(!event)return;
        event->kind=Kind::Development;event->row[0]=sequence;event->row[1]=command;
        event->row[2]=seat;event->row[3]=hp;event->row[4]=ammo;publish();
    }
    bool mode(unsigned sequence,unsigned frame,const Mode& mode) {
        if(!modes_enabled) {fail(Invalid);return false;}
        auto *event=reserve();if(!event)return false;
        event->kind=Kind::Mode;
        event->row=Policy::mode_row(sequence,frame,mode);
        publish();return true;
    }
    // Explicit barrier for tests/tools, never called by the recording loop.
    void flush() {
        auto *event=reserve();if(!event)return;
        unsigned target=++requested_flushes;event->kind=Kind::Flush;publish();
        while(flushes.load(std::memory_order_acquire)<target && !error.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    void close() {
        stop.store(true,std::memory_order_release);
        if(thread.joinable())thread.join();
        else {
            if(file.is_open()) {file.close();if(!file)fail(Io);}
            if(modes.is_open()) {modes.close();if(!modes)fail(Io);}
        }
        opened=false;queue.reset();
    }
    void clear() {
        // Caller must close first; a worker can never outlive its queue.
        if(opened)return;
        file.clear();modes.clear();modes_enabled=false;error.store(None);stop.store(false);
        read.store(0);write.store(0);flushes.store(0);requested_flushes=0;
    }
    bool is_open() const {return opened;}
    explicit operator bool() const {return error.load(std::memory_order_relaxed)==None;}
    const char *error_text() const {
        switch(error.load(std::memory_order_relaxed)) {
        case Full:return "background diagnostic queue full; capture stopped";
        case Io:return "diagnostic file write failed";
        case Start:return "cannot start diagnostic background writer";
        case Invalid:return "invalid diagnostic state size";
        default:return nullptr;
        }
    }
};
}
