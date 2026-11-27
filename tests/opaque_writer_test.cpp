/* SPDX-License-Identifier: MIT */
#include "recomp_diagnostics/opaque_writer.h"
#include "recomp_diagnostics/opaque_writer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
using namespace recomp::diagnostics;
namespace fs = std::filesystem;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " << #x << '\n';                             \
      std::abort();                                                            \
    }                                                                          \
  } while (0)
static std::string read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
static std::string utf8(const fs::path &p) {
  auto s = p.u8string();
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false, released = false;
  void wait() {
    std::unique_lock<std::mutex> l(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(l, [&] { return released; });
  }
  void entered_wait() {
    std::unique_lock<std::mutex> l(mutex);
    changed.wait(l, [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> l(mutex);
    released = true;
    changed.notify_all();
  }
};
static void queue_profile(const fs::path &root, bool release) {
  fs::create_directory(root);
  OpaqueWriter writer;
  auto p = OpaqueProfile::psp();
  p.queue_bytes = 8;
  p.queue_rows = 1;
  p.release_bytes_on_dequeue = release;
  CHECK(writer.open(root, p, {{"a", {}, false}}));
  Gate gate;
  CHECK(writer.push(
      [&](OpaqueSink &s) {
        gate.wait();
        return s.write(0, "a", 1);
      },
      {8, 1, 0}));
  gate.entered_wait();
  const bool second =
      writer.push([](OpaqueSink &s) { return s.write(0, "b", 1); }, {8, 1, 0});
  CHECK(second == release);
  if (release)
    CHECK(!writer.push([](OpaqueSink &) { return true; }, {1, 1, 0}));
  writer.request_stop();
  gate.release();
  writer.join();
  CHECK(!writer.good());
  CHECK(read(root / "a") == (release ? "ab" : "a"));
  CHECK(!writer.push([](OpaqueSink &) { return true; }, {1, 1, 0}));
  CHECK(writer.open(root, p, {{"a", {}, false}}));
  writer.close();
  CHECK(writer.good());
}
int main() {
  const auto root =
      fs::temp_directory_path() /
      fs::u8path(
          "opaque-\xC3\xA9-" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(fs::create_directory(root));
  const auto psp = OpaqueProfile::psp(), dc = OpaqueProfile::dreamcast(),
             ps2 = OpaqueProfile::ps2();
  CHECK(psp.queue_bytes == 16 * 1024 * 1024 && psp.queue_rows == 4096 &&
        !psp.release_bytes_on_dequeue);
  CHECK(psp.admitted_bytes == 64 * 1024 * 1024 &&
        psp.admitted_frames == 216000 &&
        psp.session_bytes == 192 * 1024 * 1024 && psp.retained_sessions == 0);
  CHECK(dc.queue_bytes == 1024 * 1024 && dc.release_bytes_on_dequeue &&
        dc.admitted_bytes == 64 * 1024 * 1024 &&
        dc.attachment_bytes == 64 * 1024 * 1024 && dc.retained_sessions == 4);
  CHECK(ps2.queue_rows == 4096 && ps2.queue_bytes == 0 &&
        ps2.stop_on_encoder_failure &&
        ps2.write_bytes == 32 * 1024 * 1024 - 4096 && ps2.buffer_bytes == 8192);
  {
    auto dir = root / "multi";
    fs::create_directory(dir);
    OpaqueWriter w;
    CHECK(w.open(dir, dc,
                 {{"a", {'H'}, false},
                  {"b", {}, false},
                  {"c", {}, false},
                  {"d", {}, false}}));
    const auto producer = std::this_thread::get_id();
    std::atomic<unsigned> calls{0};
    for (unsigned i = 0; i < 12; ++i)
      CHECK(w.push(
          [&, i](OpaqueSink &sink) {
            CHECK(std::this_thread::get_id() != producer);
            ++calls;
            const char value = static_cast<char>('a' + i);
            return sink.write(i % 4, &value, 1);
          },
          {1, 1, 0}));
    w.close();
    CHECK(w.good() && calls == 12);
    CHECK(read(dir / "a") == "Haei");
    CHECK(read(dir / "d") == "dhl");
    CHECK(w.attach("initial.vmu", "seed", 4));
    CHECK(read(dir / "initial.vmu") == "seed");
    CHECK(w.stats().admitted_bytes == 12 && w.stats().attachment_bytes == 4);
  }
  queue_profile(root / "psp-charge", false);
  queue_profile(root / "dc-charge", true);
  {
    auto dir = root / "budgets";
    fs::create_directory(dir);
    auto p = psp;
    p.admitted_bytes = 4;
    p.admitted_frames = 2;
    OpaqueWriter w;
    CHECK(w.open(dir, p, {{"a", {}, false}}));
    CHECK(
        w.push([](OpaqueSink &s) { return s.write(0, "abc", 3); }, {4, 3, 1}));
    CHECK(w.push([](OpaqueSink &s) { return s.write(0, "d", 1); }, {2, 1, 1}));
    CHECK(!w.push([](OpaqueSink &) { return true; }, {1, 0, 1}));
    w.close();
    CHECK(!w.good() && read(dir / "a") == "abcd");
    ArtifactBudget budget(4, 7);
    CHECK(budget.charge(4) && budget.charge(3));
    CHECK(!budget.charge(1) && !budget.charge(5));
    CHECK(budget.used() == 7);
  }
  {
    auto dir = root / "c-api";
    fs::create_directory(dir);
    auto path = utf8(dir);
    auto *w = rd_opaque_writer_create(RD_OPAQUE_PS2);
    CHECK(w);
    const rd_opaque_stream streams[] = {{"input", "H", 1, 1},
                                        {"telemetry", nullptr, 0, 1}};
    CHECK(rd_opaque_writer_open(w, path.c_str(), streams, 2, 7));
    struct Payload {
      std::atomic<unsigned> *release;
    };
    std::atomic<unsigned> released{0};
    auto encode = [](void *, rd_opaque_sink *sink) {
      return rd_opaque_sink_write(sink, 0, "x", 1);
    };
    auto destroy = [](void *p) {
      auto *value = static_cast<Payload *>(p);
      ++*value->release;
      delete value;
    };
    CHECK(rd_opaque_writer_push(w, new Payload{&released}, encode, destroy, 0,
                                0, 0));
    rd_opaque_writer_request_stop(w);
    rd_opaque_writer_join(w);
    CHECK(released == 1);
    CHECK(rd_opaque_writer_written(w) == 9 && read(dir / "input") == "Hx");
    CHECK(!rd_opaque_writer_push(w, new Payload{&released}, encode, destroy, 0,
                                 0, 0));
    CHECK(released == 2);
    CHECK(!rd_opaque_writer_open(w, path.c_str(), streams, 2, 0));
    CHECK(read(dir / "input") == "Hx");
    rd_opaque_writer_destroy(w);
    CHECK(!rd_opaque_writer_push(nullptr, new Payload{&released}, encode,
                                 destroy, 0, 0, 0));
    CHECK(released == 3);
  }
  {
    auto dir = root / "history";
    fs::create_directory(dir);
    OwnedHistoryPolicy policy;
    policy.sessions = 4;
    policy.classify = [](const fs::path &path) {
      return fs::exists(path / "owner") ? OwnedHistoryEntry::reclaimable
                                        : OwnedHistoryEntry::unowned;
    };
    policy.allowed = [](const fs::path &name, bool directory) {
      return !directory && (name == "owner" || name == "data");
    };
    for (unsigned i = 0; i < 4; ++i) {
      auto path = dir / ("session-" + std::to_string(i));
      fs::create_directory(path);
      std::ofstream(path / "owner") << "owned";
      std::ofstream(path / "data") << i;
    }
    std::ofstream(dir / "session-0" / "player-save") << "keep";
    CHECK(reserve_owned_history(dir, policy));
    CHECK(fs::exists(dir / "session-0" / "player-save") &&
          !fs::exists(dir / "session-1"));
    CHECK(!remove_owned_history_entry(dir / "session-0", policy));
    CHECK(fs::exists(dir / "session-0" / "data"));
    fs::remove(dir / "session-0" / "player-save");
    CHECK(remove_owned_history_entry(dir / "session-0", policy));
    auto lease = OwnedHistoryAdmission::acquire(dir / "admission");
    CHECK(lease);
    CHECK(!OwnedHistoryAdmission::acquire(dir / "admission"));
    std::ofstream(dir / "admission" / "unexpected") << "preserve";
    lease.reset();
    CHECK(fs::exists(dir / "admission" / "unexpected"));
    fs::remove(dir / "admission" / "unexpected");
    fs::remove(dir / "admission");
  }
  // Every path above is fixture-owned; never recurse through a link or an
  // unexpected directory. The finite known tree is removed deepest first.
  std::vector<fs::path> paths;
  for (const auto &item : fs::recursive_directory_iterator(root)) {
    CHECK(plain_owned_path(item.path()) && item.path().filename() != ".git");
    paths.push_back(item.path());
  }
  std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
    return a.native().size() > b.native().size();
  });
  for (const auto &path : paths)
    CHECK(fs::remove(path));
  CHECK(fs::remove(root));
  std::cout << "opaque native profiles, multistream FIFO, queue accounting, "
               "ownership, budgets and history passed\n";
}
