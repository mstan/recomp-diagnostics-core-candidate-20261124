#include "replay_writer.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <vector>
#define CHECK(x) do{if(!(x))std::abort();}while(0)
using namespace recomp::diagnostics;
int main(){
 std::vector<uint8_t> data(10003);for(size_t i=0;i<data.size();++i)data[i]=uint8_t(i*37);
 ReplayHash streaming;for(size_t i=0;i<data.size();i+=7)streaming.update(data.data()+i,std::min<size_t>(7,data.size()-i));CHECK(streaming.finish()==replay_hash(data.data(),data.size()));
 auto root=std::filesystem::current_path()/("replay-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directory(root);
 auto writer=ReplayWriter::create();CHECK(writer);auto path=(root/"recording.txt").string();
 CHECK(writer->begin(path,"BUILD fixture INPUT synthetic SETTINGS default",{},{}));
 CHECK(writer->line("I 0 1 2"));CHECK(writer->snapshot(data.data(),data.size(),'C'));
 ReplayWriter::Part part{data.data(),data.size(),false};CHECK(writer->frame(0,&part,1));part.reuse=true;CHECK(writer->frame(1,&part,1));
 CHECK(writer->end());CHECK(!writer->shutdown());CHECK(!writer->shutdown());
 std::ifstream in(path);std::string text((std::istreambuf_iterator<char>(in)),{});CHECK(text.find("BUILD fixture")!=std::string::npos && text.find("I 0 1 2")!=std::string::npos && text.find("H 0 ")!=std::string::npos && text.find("H 1 ")!=std::string::npos);in.close();
 CHECK(std::filesystem::file_size(path+".state")==data.size());std::ifstream state(path+".state",std::ios::binary);std::vector<uint8_t> saved((std::istreambuf_iterator<char>(state)),{});CHECK(saved==data);state.close();
 auto bad=ReplayWriter::create();CHECK(bad);CHECK(!bad->snapshot(nullptr,1,'C'));CHECK(bad->status()==ReplayWriter::Status::Invalid);bad->shutdown();writer.reset();bad.reset();std::filesystem::remove_all(root);return 0;
}
