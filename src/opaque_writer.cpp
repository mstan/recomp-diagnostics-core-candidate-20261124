/* SPDX-License-Identifier: MIT */
#include "recomp_diagnostics/opaque_writer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace recomp::diagnostics {
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t MiB = 1024 * 1024;
bool room(std::uint64_t used, std::uint64_t amount, std::uint64_t limit) {
  return amount <= std::numeric_limits<std::uint64_t>::max() - used &&
         (!limit || (used <= limit && amount <= limit - used));
}
bool flat(const std::string &name) {
  const auto p = fs::u8path(name);
  return !name.empty() && name != "." && name != ".." && p.filename() == p &&
         !p.has_root_name() && name.find(':') == std::string::npos;
}
std::string utf8_name(const fs::path &path) {
  const auto bytes = path.filename().u8string();
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
FILE *open_file(const fs::path &path, bool exclusive) {
  if (exclusive) {
#ifdef _WIN32
    int descriptor = -1;
    if (_wsopen_s(&descriptor, path.c_str(),
                  _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE))
      return nullptr;
    auto *file = _fdopen(descriptor, "wb");
    if (!file)
      _close(descriptor);
#else
    const int descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0)
      return nullptr;
    auto *file = fdopen(descriptor, "wb");
    if (!file)
      ::close(descriptor);
#endif
    return file;
  }
#ifdef _WIN32
  return _wfopen(path.c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}
bool readonly(const fs::path &path) {
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES ||
         (attributes & FILE_ATTRIBUTE_READONLY);
#else
  (void)path;
  return false;
#endif
}
} // namespace
OpaqueProfile OpaqueProfile::psp() {
  OpaqueProfile p;
  p.queue_bytes = 16 * MiB;
  p.queue_rows = 4096;
  p.admitted_bytes = 64 * MiB;
  p.admitted_frames = 216000;
  p.file_bytes = 64 * MiB;
  p.session_bytes = 192 * MiB;
  p.poll_ms = 20;
  return p;
}
OpaqueProfile OpaqueProfile::dreamcast() {
  OpaqueProfile p;
  p.queue_bytes = MiB;
  p.release_bytes_on_dequeue = true;
  p.admitted_bytes = 64 * MiB;
  p.attachment_bytes = 64 * MiB;
  p.session_bytes = 128 * MiB;
  p.retained_sessions = 4;
  return p;
}
OpaqueProfile OpaqueProfile::ps2() {
  OpaqueProfile p;
  p.queue_rows = 4096;
  p.release_bytes_on_dequeue = true;
  p.stop_on_encoder_failure = true;
  p.session_bytes = 32 * MiB;
  p.final_reserve = 4096;
  p.write_bytes = p.session_bytes - p.final_reserve;
  p.retained_sessions = 4;
  p.buffer_bytes = 8192;
  return p;
}
struct OpaqueWriter::Impl {
  enum Error { none, full, budget, io, start, codec, invalid };
  struct Row {
    Encoder encoder;
    OpaqueCharge charge;
  };
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<Row> queue;
  std::thread worker;
  std::vector<FILE *> files;
  fs::path directory;
  OpaqueProfile profile;
  OpaqueStats counters;
  std::size_t queued_bytes = 0;
  std::atomic<Error> failure{none};
  bool stopping = false, opened = false;
  void fail(Error value) {
    Error expected = none;
    failure.compare_exchange_strong(expected, value);
  }
  bool charge(std::size_t size, bool strict) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!room(counters.written_bytes, size, profile.write_bytes)) {
      if (strict)
        fail(budget);
      return false;
    }
    counters.written_bytes += size;
    return true;
  }
  bool write(unsigned stream, const void *bytes, std::size_t size) {
    if (stream >= files.size() || (!bytes && size)) {
      fail(invalid);
      return false;
    }
    if (!charge(size, true))
      return false;
    if (size && std::fwrite(bytes, 1, size, files[stream]) != size) {
      fail(io);
      return false;
    }
    return true;
  }
  bool flush() {
    bool ok = true;
    for (auto *file : files)
      if (std::fflush(file)) {
        fail(io);
        ok = false;
      }
    return ok;
  }
  void run() {
    auto flushed = std::chrono::steady_clock::now();
    for (;;) {
      Row row;
      bool have = false;
      {
        std::unique_lock<std::mutex> lock(mutex);
        if (queue.empty() && !stopping) {
          const auto deadline =
              flushed + std::chrono::milliseconds(profile.flush_ms);
          const auto poll = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(profile.poll_ms);
          wake.wait_until(lock, std::min(deadline, poll),
                          [&] { return stopping || !queue.empty(); });
        }
        if (queue.empty() && stopping)
          break;
        if (!queue.empty()) {
          row = std::move(queue.front());
          queue.pop_front();
          have = true;
          if (profile.release_bytes_on_dequeue)
            queued_bytes -= row.charge.queue_bytes;
        }
      }
      bool encoded = true;
      if (have) {
        OpaqueSink sink(this);
        try {
          encoded = row.encoder(sink);
        } catch (...) {
          encoded = false;
        }
        if (!encoded)
          fail(codec);
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!profile.release_bytes_on_dequeue)
            queued_bytes -= row.charge.queue_bytes;
          if (encoded)
            ++counters.completed;
        }
      }
      const auto now = std::chrono::steady_clock::now();
      bool flushed_ok = true;
      if (now - flushed >= std::chrono::milliseconds(profile.flush_ms)) {
        flushed_ok = flush();
        flushed = now;
      }
      if (profile.stop_on_encoder_failure &&
          ((have && !encoded) || !flushed_ok)) {
        std::lock_guard<std::mutex> lock(mutex);
        queue.clear();
        queued_bytes = 0;
        stopping = true;
        break;
      }
    }
    flush();
    for (auto *file : files)
      if (std::fclose(file))
        fail(io);
    files.clear();
  }
};
OpaqueWriter::OpaqueWriter() : impl_(new Impl) {}
OpaqueWriter::~OpaqueWriter() { close(); }
bool OpaqueWriter::open(const fs::path &directory, OpaqueProfile profile,
                        const std::vector<OpaqueStream> &streams,
                        std::uint64_t initial) {
  close();
  auto &p = *impl_;
  p.failure = Impl::none;
  p.profile = profile;
  p.directory = directory;
  p.counters = {};
  p.counters.written_bytes = initial;
  p.queued_bytes = 0;
  p.stopping = false;
  if (streams.empty() || streams.size() > 32 || !profile.poll_ms ||
      !profile.flush_ms || !room(0, initial, profile.write_bytes)) {
    p.fail(Impl::invalid);
    return false;
  }
  try {
    std::vector<std::string> names;
    for (const auto &stream : streams) {
      if (!flat(stream.name) ||
          std::find(names.begin(), names.end(), stream.name) != names.end()) {
        p.fail(Impl::invalid);
        break;
      }
      names.push_back(stream.name);
      auto *file =
          open_file(directory / fs::u8path(stream.name), stream.exclusive);
      if (!file) {
        p.fail(Impl::io);
        break;
      }
      if (profile.buffer_bytes &&
          std::setvbuf(file, nullptr, _IOFBF, profile.buffer_bytes)) {
        std::fclose(file);
        p.fail(Impl::io);
        break;
      }
      p.files.push_back(file);
      if (!p.write(static_cast<unsigned>(p.files.size() - 1),
                   stream.header.data(), stream.header.size()))
        break;
    }
    p.flush();
    if (!good()) {
      for (auto *file : p.files)
        std::fclose(file);
      p.files.clear();
      return false;
    }
    p.opened = true;
    p.worker = std::thread([&p] { p.run(); });
    return true;
  } catch (...) {
    p.fail(Impl::start);
    p.opened = false;
    for (auto *file : p.files)
      std::fclose(file);
    p.files.clear();
    return false;
  }
}
bool OpaqueWriter::push(Encoder encoder, OpaqueCharge charge) {
  auto &p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.opened || p.stopping || p.failure != Impl::none)
    return false;
  if (!encoder || (!charge.queue_bytes && !p.profile.queue_rows)) {
    p.fail(Impl::invalid);
    return false;
  }
  if (charge.queue_bytes >
          std::numeric_limits<std::size_t>::max() - p.queued_bytes ||
      (p.profile.queue_rows && p.queue.size() >= p.profile.queue_rows) ||
      !room(p.queued_bytes, charge.queue_bytes, p.profile.queue_bytes)) {
    p.fail(Impl::full);
    return false;
  }
  if (!room(p.counters.admitted_bytes, charge.admitted_bytes,
            p.profile.admitted_bytes) ||
      !room(p.counters.admitted_frames, charge.frames,
            p.profile.admitted_frames)) {
    p.fail(Impl::budget);
    return false;
  }
  try {
    p.queue.push_back({std::move(encoder), charge});
  } catch (...) {
    p.fail(Impl::start);
    return false;
  }
  p.queued_bytes += charge.queue_bytes;
  p.counters.admitted_bytes += charge.admitted_bytes;
  p.counters.admitted_frames += charge.frames;
  p.wake.notify_one();
  return true;
}
void OpaqueWriter::request_stop() {
  auto &p = *impl_;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.stopping = true;
  }
  p.wake.notify_all();
}
void OpaqueWriter::join() {
  auto &p = *impl_;
  if (p.worker.joinable())
    p.worker.join();
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.opened = false;
    p.queue.clear();
    p.queued_bytes = 0;
  }
}
void OpaqueWriter::close() {
  request_stop();
  join();
}
bool OpaqueWriter::good() const { return impl_->failure == Impl::none; }
void OpaqueWriter::invalidate() { impl_->fail(Impl::codec); }
bool OpaqueWriter::is_open() const {
  auto &p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.opened && !p.stopping;
}
const char *OpaqueWriter::error() const {
  switch (impl_->failure.load()) {
  case Impl::full:
    return "bounded diagnostic queue full";
  case Impl::budget:
    return "diagnostic session budget exhausted";
  case Impl::io:
    return "diagnostic file I/O failed";
  case Impl::start:
    return "diagnostic worker allocation/start failed";
  case Impl::codec:
    return "diagnostic encoder failed";
  case Impl::invalid:
    return "invalid diagnostic stream or charge";
  default:
    return nullptr;
  }
}
OpaqueStats OpaqueWriter::stats() const {
  auto &p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.counters;
}
bool OpaqueWriter::attach(const std::string &name, const void *bytes,
                          std::size_t size) {
  auto &p = *impl_;
  if (!flat(name) || (!bytes && size))
    return false;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!p.profile.attachment_bytes ||
        !room(p.counters.attachment_bytes, size, p.profile.attachment_bytes)) {
      p.fail(Impl::budget);
      return false;
    }
  }
  auto *file = open_file(p.directory / fs::u8path(name), false);
  if (!file) {
    p.fail(Impl::io);
    return false;
  }
  const bool wrote = !size || std::fwrite(bytes, 1, size, file) == size;
  const bool closed = std::fclose(file) == 0;
  if (!wrote || !closed) {
    p.fail(Impl::io);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.counters.attachment_bytes += size;
  }
  return true;
}
bool OpaqueSink::write(unsigned stream, const void *bytes, std::size_t size) {
  return static_cast<OpaqueWriter::Impl *>(state_)->write(stream, bytes, size);
}
bool OpaqueSink::charge_auxiliary(std::size_t size) {
  return static_cast<OpaqueWriter::Impl *>(state_)->charge(size, false);
}
bool ArtifactBudget::charge(std::uint64_t bytes) {
  if ((file_limit_ && bytes > file_limit_) || !room(used_, bytes, total_limit_))
    return false;
  used_ += bytes;
  return true;
}
bool plain_owned_path(const fs::path &path) {
  std::error_code ec;
  const auto st = fs::symlink_status(path, ec);
  if (ec || fs::is_symlink(st))
    return false;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
    return false;
#endif
  return true;
}
unsigned recorder_process_id() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<unsigned>(getpid());
#endif
}
bool recorder_process_alive(unsigned pid) {
  if (!pid)
    return true;
#ifdef _WIN32
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process)
    return GetLastError() != ERROR_INVALID_PARAMETER;
  DWORD code = STILL_ACTIVE;
  const bool alive =
      !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
#else
  return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}
namespace {
bool collect(const fs::path &base, const fs::path &directory, unsigned depth,
             const OwnedHistoryPolicy &policy, std::vector<fs::path> &files,
             std::vector<fs::path> &directories) {
  for (const auto &entry : fs::directory_iterator(directory)) {
    const auto path = entry.path();
    if (!plain_owned_path(path) || (policy.protect_readonly && readonly(path)))
      return false;
    const bool is_directory = entry.is_directory();
    if (!policy.allowed(path.lexically_relative(base), is_directory))
      return false;
    if (is_directory) {
      if (!depth || !collect(base, path, depth - 1, policy, files, directories))
        return false;
      directories.push_back(path);
    } else if (entry.is_regular_file())
      files.push_back(path);
    else
      return false;
  }
  return true;
}
} // namespace
bool reserve_owned_history(const fs::path &root,
                           const OwnedHistoryPolicy &policy) {
  if (!policy.sessions)
    return true;
  if (!policy.classify || !policy.allowed || !plain_owned_path(root))
    return false;
  std::vector<fs::path> owned, reclaimable;
  for (const auto &entry : fs::directory_iterator(root)) {
    const auto path = entry.path();
    if (utf8_name(path).rfind(policy.prefix, 0) || !plain_owned_path(path) ||
        !entry.is_directory())
      continue;
    const auto kind = policy.classify(path);
    if (kind == OwnedHistoryEntry::unowned)
      continue;
    owned.push_back(path);
    if (kind == OwnedHistoryEntry::reclaimable)
      reclaimable.push_back(path);
  }
  std::sort(reclaimable.begin(), reclaimable.end());
  std::size_t count = owned.size();
  for (const auto &path : reclaimable) {
    if (count < policy.sessions)
      break;
    if (!remove_owned_history_entry(path, policy))
      continue;
    --count;
  }
  return count < policy.sessions;
}
bool remove_owned_history_entry(const fs::path &path,
                                const OwnedHistoryPolicy &policy) {
  if (!policy.allowed || !plain_owned_path(path))
    return false;
  std::vector<fs::path> files, directories;
  if (!collect(path, path, policy.directory_depth, policy, files, directories))
    return false;
  for (const auto &file : files)
    fs::remove(file);
  for (const auto &directory : directories)
    fs::remove(directory);
  return fs::remove(path);
}
std::unique_ptr<OwnedHistoryAdmission>
OwnedHistoryAdmission::acquire(const fs::path &path) {
  auto owner =
      std::unique_ptr<OwnedHistoryAdmission>(new OwnedHistoryAdmission(path));
  if (!fs::create_directory(path))
    return {};
  owner->held_ = true;
  return owner;
}
OwnedHistoryAdmission::~OwnedHistoryAdmission() {
  if (held_ && plain_owned_path(path_)) {
    std::error_code error;
    fs::remove(path_, error);
  }
}
} // namespace recomp::diagnostics
