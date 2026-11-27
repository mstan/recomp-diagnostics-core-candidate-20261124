// SPDX-License-Identifier: MIT
#include <recomp_diagnostics/session.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#define CHECK(x) do { if(!(x)) std::abort(); } while(0)
using namespace recomp::diagnostics;
namespace fs=std::filesystem;
using Bytes=DiagnosticSession::Bytes;
static std::string read(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
static void delay(const char* value){
#ifdef _WIN32
    _putenv_s("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US",value);
#else
    setenv("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US",value,1);
#endif
}
int main(){
    auto gba=DiagnosticSession::gba_profile("root","fixture");
    auto gb=DiagnosticSession::gb_profile("root","fixture");
    CHECK(gba.segment_frames==600&&gba.history_segments==360&&gba.history_bytes==256u*1024u*1024u&&gba.overflow==DiagnosticSession::Overflow::RecoverWithCheckpoint);
    CHECK(gb.segment_frames==600&&gb.history_segments==360&&gb.history_bytes==64u*1024u*1024u&&gb.queue_bytes==16u*1024u*1024u&&gb.overflow==DiagnosticSession::Overflow::StopRecording);
    auto root=fs::current_path()/("session-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    {
        DiagnosticSession::Options o;o.root=root/"stream";o.stem="fixture";o.segment_frames=2;o.history_segments=2;o.history_bytes=24;o.segment_checkpoint_bytes=12;
        DiagnosticSession s(o);CHECK(s.recording());
        auto header=[](unsigned n){return "FORMAT 1\nSEGMENT "+std::to_string(n)+"\n";};
        CHECK(s.needs_segment(1,8));CHECK(s.begin_segment(header,{},{}));
        CHECK(s.snapshot("12345678",8,'C'));s.accept_epoch(1);
        CHECK(!s.discontinuity(1)&&s.discontinuity(2));CHECK(s.needs_segment(2,8));
        CHECK(s.line("I 1\n"));CHECK(s.frame(1,nullptr,0));s.completed_frame();
        CHECK(!s.needs_segment(1,8));s.completed_frame();CHECK(s.needs_segment(1,8));
        CHECK(s.begin_segment(header,{},{}));CHECK(s.snapshot("abcdefgh",8,'C'));
        CHECK(s.begin_segment(header,{},{}));CHECK(s.snapshot("abcdefgh",8,'C'));
        CHECK(s.oldest_segment()==2);CHECK(!s.shutdown("E 3\n"));
        CHECK(!fs::exists(o.root/"fixture-000001.replay"));CHECK(!fs::exists(o.root/"fixture-000001.replay.state"));
        CHECK(read(o.root/"fixture-000003.replay").find("E 3\n")!=std::string::npos);
    }
    {
        DiagnosticSession::Options o;o.root=root/"blob";o.stem="fixture";o.blob_segments=true;o.first_segment=0;o.digits=12;o.segment_frames=2;o.history_segments=2;o.history_bytes=1024;o.blob_header_bytes=2;o.queue_bytes=16384;o.overflow=DiagnosticSession::Overflow::StopRecording;o.log_header="sanitized metadata\n";
        DiagnosticSession s(o);Bytes checkpoint={0x40};
        auto capture=[&]{return checkpoint;};
        auto codec=[](const Bytes& state,const Bytes& inputs,std::uint64_t epoch,unsigned frames,const Bytes& final){Bytes out={std::uint8_t(epoch),std::uint8_t(frames)};out.insert(out.end(),state.begin(),state.end());out.insert(out.end(),inputs.begin(),inputs.end());out.insert(out.end(),final.begin(),final.end());return out;};
        s.capture_begin(1,{0x10},capture,codec);s.capture_end(capture);
        s.capture_begin(1,{0xff},capture,codec); // input discarded by restore
        s.capture_begin(2,{0x20},capture,codec);s.capture_end(capture);s.finish_blob(capture);s.drain();
        CHECK(read(o.root/"segment-fixture-000000000000.replay")==std::string("\1\1\x40\x10",4));
        CHECK(read(o.root/"segment-fixture-000000000001.replay")==std::string("\2\1\x40\x20\x40",5));
        std::ofstream(o.root/"private.sav")<<"PRIVATE_SAVE_MUST_NOT_ENTER_REPORT";
        s.log("local log");std::uint32_t pixel=0x123456;
        auto report=s.report({1,2,3},&pixel,1,1,1,"sanitized report");CHECK(!report.empty());s.drain();
        const auto archive=read(report);CHECK(archive.substr(0,2)=="PK");CHECK(archive.find("sanitized report")!=std::string::npos);CHECK(archive.find("PRIVATE_SAVE_MUST_NOT_ENTER_REPORT")==std::string::npos);CHECK(!fs::exists(fs::path(report.string()+".partial")));
        for(int i=0;i<5;++i){s.capture_begin(2,{std::uint8_t(i)},capture,codec);s.capture_end(capture);s.finish_blob(capture);s.drain();}
        unsigned count=0;for(const auto& e:fs::directory_iterator(o.root))if(e.path().extension()==".replay")++count;CHECK(count<=2);
    }
    {
        DiagnosticSession::Options o;o.root=root/"tiny";o.stem="segment-tiny";o.blob_segments=true;o.segment_frames=1;o.queue_bytes=1;o.overflow=DiagnosticSession::Overflow::StopRecording;
        DiagnosticSession s(o);auto capture=[]{return Bytes{1};};auto codec=[](const Bytes&,const Bytes&,std::uint64_t,unsigned,const Bytes&){return Bytes{1};};
        s.capture_begin(0,{1},capture,codec);s.capture_end(capture);CHECK(!s.recording());CHECK(s.message().find("queue")!=std::string::npos);
    }
    delay("250000");
    {
        DiagnosticSession::Options o;o.root=root/"gap";o.stem="fixture";
        DiagnosticSession s(o);CHECK(s.begin_segment([](unsigned){return "FORMAT 1\n";},{},{}));
        Bytes state(8u*1024u*1024u,0x55);while(s.snapshot(state.data(),state.size(),'C')){}
        CHECK(s.has_gap());CHECK(!s.segment_open());s.drain();while(!s.ready())std::this_thread::yield();
        CHECK(s.begin_segment([](unsigned){return "FORMAT 1\n";},{},{}));CHECK(s.snapshot("new",3,'R'));CHECK(!s.shutdown("E 1\n"));
        CHECK(read(o.root/"fixture-000001.replay").find("G\n")!=std::string::npos);
    }
    delay("");fs::remove_all(root);
}
