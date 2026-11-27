#include "recomp_diagnostics/replay_c.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
namespace fs=std::filesystem;
static std::string utf8(const fs::path&p) { auto u=p.u8string(); return {u.begin(),u.end()}; }
static std::string read(const fs::path&p) {std::ifstream f(p,std::ios::binary);return {(std::istreambuf_iterator<char>(f)),{}};}
static void write(const fs::path&p,const std::string&s) {std::ofstream f(p,std::ios::binary);f<<s;CHECK(f.good());}
int main() {
 auto root=fs::current_path()/fs::u8path("replay-profiles-\xe6\x97\xa5-\xc3\xa9");
 CHECK(!fs::exists(root));fs::create_directory(root);
 auto config=utf8(root/"source.ini"),mods=utf8(root/"source.toml");
 write(fs::u8path(config),"config bytes\n");write(fs::u8path(mods),"mods bytes\n");
 auto raw=utf8(root/"nes.cycreplay");write(fs::u8path(raw+".state"),"old oversized state");
 write(root/"remove-me","old");
 auto w=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,16*1024*1024);CHECK(w);
 CHECK(recomp_replay_begin_files(w,raw.c_str(),"NES header\n",config.c_str(),mods.c_str(),"effective\n"));
 CHECK(recomp_replay_snapshot_raw(w,"\0\1\2",3));
 CHECK(recomp_replay_line(w,"P arbitrary\nD row\nZ row\nI row\nH engine-hashes\n"));
 auto remove=utf8(root/"remove-me");CHECK(recomp_replay_remove(w,remove.c_str()));
 // Recording must flush even when the simulation produces no further rows.
 const std::string expected="NES header\nP arbitrary\nD row\nZ row\nI row\nH engine-hashes\n";
 while(read(fs::u8path(raw))!=expected) {CHECK(!recomp_replay_error(w));std::this_thread::sleep_for(std::chrono::milliseconds(10));}
 CHECK(recomp_replay_end(w));CHECK(!recomp_replay_destroy(w));
 CHECK(read(fs::u8path(raw+".state"))==std::string("\0\1\2",3));CHECK(!fs::exists(root/"remove-me"));
 CHECK(read(fs::u8path(raw+".config.ini"))=="config bytes\n");
 CHECK(read(fs::u8path(raw+".mods.toml"))=="mods bytes\n");
 // Snes32 must replace segments in place, with four independent FNV32 hashes.
 auto snes=utf8(root/"snes.replay");write(fs::u8path(snes),"foreign previous text");
 w=recomp_replay_create_profile(RECOMP_REPLAY_SNES32,0);CHECK(w);
 CHECK(recomp_replay_begin_files(w,snes.c_str(),"SNESREPLAY 1\n","","",""));
 CHECK(recomp_replay_snapshot(w,"abc",3));
 RecompReplayPart parts[4]={{"abc",3,false},{"",0,false},{"x",1,false},{"z",1,false}};
 CHECK(recomp_replay_frame_parts(w,9,parts,4));CHECK(recomp_replay_end(w));CHECK(!recomp_replay_destroy(w));
 CHECK(read(fs::u8path(snes))=="SNESREPLAY 1\nS 0 3 1a47e90b\nH 9 1a47e90b 811c9dc5 fd0c5087 ff0c53ad\n");
 CHECK(!fs::exists(fs::u8path(snes+".previous-1")));
 // The original C default retains Genesis history rotation and FNV snapshots.
 auto genesis=utf8(root/"genesis.replay");
 for(int i=0;i<2;++i) {w=recomp_replay_create();CHECK(w);CHECK(recomp_replay_begin(w,genesis.c_str(),"GENESISREPLAY 1\n","","",""));CHECK(recomp_replay_snapshot(w,"abc",3));CHECK(recomp_replay_end(w));CHECK(!recomp_replay_destroy(w));}
 CHECK(read(fs::u8path(genesis+".previous-1"))=="GENESISREPLAY 1\nS 0 3 1a47e90b\n");
 CHECK(!recomp_replay_create_profile((RecompReplayFormat)99,0));CHECK(!recomp_replay_create_profile(RECOMP_REPLAY_SNES32,33*1024*1024));
 w=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,0);CHECK(w);CHECK(!recomp_replay_snapshot_raw(w,nullptr,1));CHECK(recomp_replay_error(w));CHECK(!recomp_replay_line(w,"later"));CHECK(recomp_replay_destroy(w));
#ifdef _WIN32
 _putenv_s("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US","200000");
#else
 setenv("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US","200000",1);
#endif
 w=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,4096);CHECK(w);CHECK(recomp_replay_begin_with_sidecars(w,raw.c_str(),"bounded\n",nullptr,0,nullptr,0));
 std::string row(2500,'a');CHECK(recomp_replay_line(w,row.c_str()));CHECK(!recomp_replay_line(w,row.c_str()));CHECK(recomp_replay_error(w));CHECK(!recomp_replay_end(w));CHECK(recomp_replay_destroy(w));
#ifdef _WIN32
 _putenv_s("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US","");
#else
 unsetenv("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US");
#endif
 w=recomp_replay_create_profile(RECOMP_REPLAY_HASHED64,0);CHECK(w);auto bad=utf8(root/"absent"/"file");CHECK(recomp_replay_begin_with_sidecars(w,bad.c_str(),"io\n",nullptr,0,nullptr,0));CHECK(recomp_replay_destroy(w));
 fs::remove_all(root);return 0;
}
