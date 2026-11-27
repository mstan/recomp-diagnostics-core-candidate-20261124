// SPDX-License-Identifier: MIT
// Storage/report machinery derived from gbcllr diagnostics (owner MIT grant).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <recomp_diagnostics/session.hpp>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace recomp::diagnostics {
namespace {
using Bytes = DiagnosticSession::Bytes;
namespace fs = std::filesystem;
void u32(Bytes &b, std::uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    b.push_back(std::uint8_t(n >> (8 * i)));
}
Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }
std::string stamp() {
  auto now = std::chrono::system_clock::now();
  auto value = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &value);
#else
  gmtime_r(&value, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y%m%d-%H%M%S") << '-'
      << std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
             .count();
  return out.str();
}
std::string number(std::uint64_t n, unsigned digits) {
  std::ostringstream s;
  s << std::setfill('0') << std::setw(digits) << n;
  return s.str();
}
std::string utf8(const fs::path &p) {
  auto s = p.u8string();
  return {reinterpret_cast<const char *>(s.data()), s.size()};
}
void write(const fs::path &p, const Bytes &b) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f || !f.write(reinterpret_cast<const char *>(b.data()),
                     std::streamsize(b.size())))
    throw std::runtime_error("diagnostics storage write failed");
}
Bytes read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("diagnostics report input unavailable");
  auto n = f.tellg();
  if (n < 0 || std::uint64_t(n) > 128u * 1024u * 1024u)
    throw std::runtime_error("diagnostics report input too large");
  Bytes b(static_cast<std::size_t>(n));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(b.data()), std::streamsize(b.size())))
    throw std::runtime_error("diagnostics report read failed");
  return b;
}
std::uint32_t crc(const Bytes &b) {
  std::uint32_t c = ~0u;
  for (auto v : b) {
    c ^= v;
    for (unsigned i = 0; i < 8; ++i)
      c = (c >> 1) ^ (0xedb88320u & std::uint32_t(-int(c & 1)));
  }
  return ~c;
}
// Store-only ZIP, no external dependency; same published GB report container.
struct Zip {
  std::ofstream out;
  Bytes central;
  std::uint32_t offset = 0, count = 0;
  explicit Zip(const fs::path &p) : out(p, std::ios::binary | std::ios::trunc) {
    if (!out)
      throw std::runtime_error("cannot create diagnostic report");
  }
  void put(std::string_view name, const Bytes &data) {
    if (data.size() > 128u * 1024u * 1024u || name.size() > 65535 ||
        count == 65535 ||
        data.size() + name.size() + 30 >
            std::numeric_limits<std::uint32_t>::max() - offset)
      throw std::runtime_error("diagnostic entry too large");
    Bytes h;
    u32(h, 0x04034b50);
    h.insert(h.end(), {20, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    auto sum = crc(data);
    u32(h, sum);
    u32(h, std::uint32_t(data.size()));
    u32(h, std::uint32_t(data.size()));
    h.push_back(std::uint8_t(name.size()));
    h.push_back(std::uint8_t(name.size() >> 8));
    h.insert(h.end(), {0, 0});
    h.insert(h.end(), name.begin(), name.end());
    out.write(reinterpret_cast<const char *>(h.data()),
              std::streamsize(h.size()));
    out.write(reinterpret_cast<const char *>(data.data()),
              std::streamsize(data.size()));
    if (!out)
      throw std::runtime_error("diagnostic ZIP write failed");
    u32(central, 0x02014b50);
    central.insert(central.end(), {20, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    u32(central, sum);
    u32(central, std::uint32_t(data.size()));
    u32(central, std::uint32_t(data.size()));
    central.push_back(std::uint8_t(name.size()));
    central.push_back(std::uint8_t(name.size() >> 8));
    central.insert(central.end(), 12, 0);
    u32(central, offset);
    central.insert(central.end(), name.begin(), name.end());
    offset += std::uint32_t(h.size() + data.size());
    ++count;
  }
  void close() {
    out.write(reinterpret_cast<const char *>(central.data()),
              std::streamsize(central.size()));
    Bytes e;
    u32(e, 0x06054b50);
    u32(e, 0);
    e.push_back(std::uint8_t(count));
    e.push_back(std::uint8_t(count >> 8));
    e.push_back(std::uint8_t(count));
    e.push_back(std::uint8_t(count >> 8));
    u32(e, std::uint32_t(central.size()));
    u32(e, offset);
    e.insert(e.end(), {0, 0});
    out.write(reinterpret_cast<const char *>(e.data()),
              std::streamsize(e.size()));
    out.close();
    if (!out)
      throw std::runtime_error("diagnostic ZIP finalization failed");
  }
};
bool owned(const fs::directory_entry &e, std::string_view prefix,
           std::string_view suffix) {
  if (!e.is_regular_file() || e.is_symlink())
    return false;
  auto n = e.path().filename().string();
  return n.size() >= prefix.size() + suffix.size() &&
         n.compare(0, prefix.size(), prefix) == 0 &&
         n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0;
}
void prune_flat(const fs::path &root, std::string_view prefix,
                std::string_view suffix, unsigned keep) {
  std::vector<fs::path> paths;
  for (const auto &e : fs::directory_iterator(root))
    if (owned(e, prefix, suffix))
      paths.push_back(e.path());
  std::sort(paths.begin(), paths.end());
  while (paths.size() > keep) {
    fs::remove(paths.front());
    paths.erase(paths.begin());
  }
}
} // namespace

struct DiagnosticSession::Impl {
  struct Entry {
    fs::path path;
    std::size_t size = 0;
    std::vector<fs::path> files;
    unsigned id = 0;
  };
  struct Blob {
    Bytes checkpoint, inputs;
    std::uint64_t epoch = 0;
    unsigned frames = 0;
    std::size_t input_bytes = 0;
    bool pending = false;
    BlobCodec encode;
  };
  Options opt;
  std::unique_ptr<ReplayWriter> writer;
  std::deque<Entry> history;
  std::size_t history_bytes = 0, checkpoint_bytes = 0;
  unsigned index = 0, rows = 0;
  std::uint64_t epoch = 0, gaps = 0, report_id = 0;
  bool open = false, stream_open = false, gap = false, gap_marker = false,
       active = false;
  std::string failure;
  std::unique_ptr<Blob> blob;
  fs::path log_path;
  std::size_t log_bytes = 0;
  mutable std::mutex message_mutex;
  std::string message = "Local diagnostics ready.";
  explicit Impl(Options o) : opt(std::move(o)) {
    opt.segment_frames = std::clamp(opt.segment_frames, 1u, 600u);
    index = opt.first_segment - 1;
    try {
      if (opt.stem.empty() ||
          fs::path(opt.stem).filename() != fs::path(opt.stem))
        throw std::runtime_error("invalid diagnostic session name");
      fs::create_directories(opt.root);
      if (opt.blob_segments) {
        for (const auto &e : fs::directory_iterator(opt.root))
          if (owned(e, "segment-", ".replay"))
            history.push_back(
                {e.path(), std::size_t(e.file_size()), {e.path()}, 0});
        std::sort(history.begin(), history.end(),
                  [](const auto &a, const auto &b) { return a.path < b.path; });
        for (const auto &e : history)
          history_bytes += e.size;
        while (!history.empty() && (history.size() > opt.history_segments ||
                                    history_bytes > opt.history_bytes)) {
          history_bytes -= history.front().size;
          fs::remove(history.front().path);
          history.pop_front();
        }
        log_path = opt.root / ("session-" + opt.stem + ".log");
        write(log_path, bytes(opt.log_header));
        prune_flat(opt.root, "session-", ".log", 8);
      }
      writer = ReplayWriter::create_with_capacity(
          ReplayWriter::Format::Hashed64,
          std::max<std::size_t>(1024, opt.queue_bytes));
      if (!writer)
        throw std::runtime_error("diagnostic storage worker unavailable");
      active = opt.recording;
    } catch (const std::exception &e) {
      fail(e.what());
    }
  }
  void fail(std::string text) {
    active = false;
    open = false;
    gap = false;
    failure = std::move(text);
    std::lock_guard lock(message_mutex);
    message = "Diagnostics stopped: " + failure;
    std::fprintf(stderr, "%s\n", message.c_str());
  }
  bool accepted(bool ok) {
    if (ok)
      return true;
    if (writer && writer->status() != Status::Io &&
        writer->status() != Status::Invalid) {
      if (opt.overflow == Overflow::RecoverWithCheckpoint) {
        ++gaps;
        gap = true;
        gap_marker = gap_marker || stream_open;
        open = false;
      } else
        fail("background writer queue is full.");
    } else
      fail(writer && writer->error() ? writer->error()
                                     : "storage worker unavailable");
    return false;
  }
  bool post(std::function<void()> job, std::size_t cost, bool manual = false) {
    if (!writer || (!active && !manual))
      return false;
    if (cost > opt.queue_bytes) {
      return accepted(false);
    }
    return accepted(writer->post(std::move(job), cost));
  }
  bool evict(bool keep_latest) {
    while (history.size() > (keep_latest ? 1u : 0u) &&
           (history.size() > opt.history_segments ||
            history_bytes > opt.history_bytes)) {
      auto &e = history.front();
      for (const auto &f : e.files)
        if (!accepted(writer->remove(utf8(f))))
          return false;
      history_bytes -= e.size;
      history.pop_front();
    }
    return true;
  }
  fs::path path(unsigned id) const {
    return opt.root / ((opt.blob_segments ? "segment-" : "") + opt.stem + "-" +
                       number(id, opt.digits) + ".replay");
  }
  void close_blob(Bytes final) {
    if (!blob || !blob->frames) {
      blob.reset();
      return;
    }
    auto data = std::shared_ptr<Blob>(blob.release());
    if (data->pending)
      final.clear();
    const auto id = ++index;
    const auto dest = path(id);
    const auto cost = data->checkpoint.size() + data->inputs.size() +
                      final.size() + opt.blob_header_bytes + 1024;
    data->inputs.resize(std::size_t(data->frames) * data->input_bytes);
    const auto stored =
        data->checkpoint.size() + data->inputs.size() + opt.blob_header_bytes;
    if (!post(
            [data, final = std::move(final), dest] {
              write(dest, data->encode(data->checkpoint, data->inputs,
                                       data->epoch, data->frames, final));
            },
            cost))
      return;
    history.push_back({dest, stored, {dest}, id});
    history_bytes += stored;
    evict(false);
    open = false;
    rows = 0;
  }
};

DiagnosticSession::Options DiagnosticSession::gba_profile(fs::path root,
                                                          std::string stem) {
  Options o;
  o.root = std::move(root);
  o.stem = std::move(stem);
  return o;
}
DiagnosticSession::Options DiagnosticSession::gb_profile(fs::path root,
                                                         std::string stem) {
  Options o;
  o.root = std::move(root);
  o.stem = std::move(stem);
  o.blob_segments = true;
  o.first_segment = 0;
  o.digits = 12;
  o.queue_bytes = 16u * 1024u * 1024u;
  o.history_bytes = 64u * 1024u * 1024u;
  o.overflow = Overflow::StopRecording;
  return o;
}
DiagnosticSession::DiagnosticSession(Options o)
    : impl_(std::make_unique<Impl>(std::move(o))) {}
DiagnosticSession::~DiagnosticSession() { shutdown(); }
bool DiagnosticSession::begin_segment(
    const std::function<std::string(unsigned)> &header,
    const std::vector<Sidecar> &copies, const std::vector<Sidecar> &texts) {
  auto &p = *impl_;
  if (!ready())
    return false;
  if (p.stream_open && !end())
    return false;
  unsigned id = p.index + 1;
  auto dest = p.path(id);
  std::vector<Sidecar> c, t;
  std::vector<fs::path> files = {dest, fs::path(utf8(dest) + ".state")};
  for (const auto &s : copies) {
    c.push_back({s.source, utf8(dest) + s.dest});
    files.emplace_back(c.back().dest);
  }
  for (const auto &s : texts) {
    t.push_back({s.source, utf8(dest) + s.dest});
    files.emplace_back(t.back().dest);
  }
  if (!p.accepted(p.writer->begin(utf8(dest), header(id), c, t)))
    return false;
  p.index = id;
  p.rows = 0;
  p.checkpoint_bytes = 0;
  p.open = p.stream_open = true;
  p.history.push_back({dest, 0, std::move(files), id});
  return p.evict(true);
}
bool DiagnosticSession::snapshot(const void *b, std::size_t n, char kind) {
  auto &p = *impl_;
  if (!p.writer || !p.active || !p.open || p.history.empty())
    return false;
  if (n >= p.opt.queue_bytes / 2) {
    p.fail("machine checkpoint exceeds the replay queue");
    return false;
  }
  // Charge before eviction, then enqueue the checkpoint in file order.
  p.history.back().size += n;
  p.history_bytes += n;
  if (!p.evict(true) || !p.accepted(p.writer->snapshot(b, n, kind))) {
    p.history.back().size -= n;
    p.history_bytes -= n;
    return false;
  }
  p.checkpoint_bytes += n;
  return true;
}
bool DiagnosticSession::line(const std::string &s) {
  auto &p = *impl_;
  return p.writer && p.active && p.open && p.accepted(p.writer->line(s));
}
bool DiagnosticSession::frame(std::uint64_t f, const Part *b, unsigned n) {
  auto &p = *impl_;
  return p.writer && p.active && p.open && p.accepted(p.writer->frame(f, b, n));
}
bool DiagnosticSession::end() {
  auto &p = *impl_;
  if (!p.stream_open)
    return true;
  if (!p.accepted(p.writer->end()))
    return false;
  p.stream_open = p.open = false;
  return true;
}
bool DiagnosticSession::ready() {
  auto &p = *impl_;
  if (!recording())
    return false;
  if (!p.gap)
    return true;
  if (p.writer->free_bytes() < p.opt.queue_bytes / 2)
    return false;
  if (p.gap_marker) {
    if (!p.accepted(p.writer->line("G\n")))
      return false;
    p.gap_marker = false;
  }
  p.gap = false;
  return true;
}
bool DiagnosticSession::discontinuity(std::uint64_t e) const {
  return impl_->open && e != impl_->epoch;
}
bool DiagnosticSession::needs_segment(std::uint64_t e,
                                      std::size_t estimate) const {
  auto &p = *impl_;
  auto limit = std::min(p.opt.segment_checkpoint_bytes, p.opt.history_bytes);
  return !p.open || p.rows >= p.opt.segment_frames ||
         (discontinuity(e) &&
          (estimate > limit || p.checkpoint_bytes > limit - estimate));
}
void DiagnosticSession::accept_epoch(std::uint64_t e) { impl_->epoch = e; }
std::uint64_t DiagnosticSession::epoch() const { return impl_->epoch; }
void DiagnosticSession::completed_frame() { ++impl_->rows; }
bool DiagnosticSession::segment_open() const { return impl_->open; }
unsigned DiagnosticSession::segment() const { return impl_->index; }
unsigned DiagnosticSession::oldest_segment() const {
  return impl_->history.empty() ? impl_->index : impl_->history.front().id;
}
std::uint64_t DiagnosticSession::gaps() const { return impl_->gaps; }
bool DiagnosticSession::has_gap() const { return impl_->gap; }
std::string DiagnosticSession::segment_path() const {
  return utf8(impl_->path(impl_->index));
}
void DiagnosticSession::capture_begin(std::uint64_t e, Bytes input,
                                      const Checkpoint &capture,
                                      const BlobCodec &encode) {
  auto &p = *impl_;
  if (!recording())
    return;
  try {
    if (p.blob && p.blob->epoch != e)
      p.close_blob({});
    if (!recording())
      return;
    if (!p.blob) {
      p.blob = std::make_unique<Impl::Blob>();
      p.blob->checkpoint = capture();
      p.blob->epoch = e;
      p.blob->encode = encode;
      p.blob->input_bytes = input.size();
      p.rows = 0;
      p.open = true;
    }
    if (input.size() != p.blob->input_bytes || p.blob->pending)
      throw std::runtime_error("invalid diagnostic input boundary");
    p.blob->pending = true;
    p.blob->inputs.insert(p.blob->inputs.end(), input.begin(), input.end());
    p.epoch = e;
  } catch (const std::exception &x) {
    p.fail(x.what());
  }
}
void DiagnosticSession::capture_end(const Checkpoint &capture) {
  auto &p = *impl_;
  if (!p.blob || !recording())
    return;
  try {
    p.blob->pending = false;
    ++p.blob->frames;
    ++p.rows;
    if (p.blob->frames >= p.opt.segment_frames)
      p.close_blob(capture());
  } catch (const std::exception &x) {
    p.fail(x.what());
  }
}
void DiagnosticSession::finish_blob(const Checkpoint &capture) {
  auto &p = *impl_;
  try {
    if (p.blob)
      p.close_blob(capture());
  } catch (const std::exception &x) {
    p.fail(x.what());
  }
}
void DiagnosticSession::log(std::string_view text) {
  auto &p = *impl_;
  if (p.log_path.empty())
    return;
  try {
    auto line = stamp() + " " + std::string(text.substr(0, 4096)) + "\n";
    auto cost = line.size();
    p.post(
        [&p, line = std::move(line)] {
          if (p.log_bytes > 1024u * 1024u)
            return;
          std::ofstream f(p.log_path, std::ios::binary | std::ios::app);
          if (!f || !f.write(line.data(), std::streamsize(line.size())))
            throw std::runtime_error("session log write failed");
          p.log_bytes += line.size();
        },
        cost, true);
  } catch (const std::exception &x) {
    p.fail(x.what());
  }
}
std::filesystem::path DiagnosticSession::report(Bytes state,
                                                const std::uint32_t *pixels,
                                                std::size_t count,
                                                unsigned width, unsigned height,
                                                std::string text) {
  auto &p = *impl_;
  try {
    Bytes image;
    if (pixels && width && height && width <= 4096 && height <= 4096 &&
        count == std::size_t(width) * height) {
      image = bytes("P6\n" + std::to_string(width) + " " +
                    std::to_string(height) + "\n255\n");
      image.reserve(image.size() + count * 3);
      for (std::size_t i = 0; i < count; ++i) {
        auto v = pixels[i];
        image.push_back(std::uint8_t(v >> 16));
        image.push_back(std::uint8_t(v >> 8));
        image.push_back(std::uint8_t(v));
      }
    }
    auto dest = p.opt.root / ("bug-report-" + p.opt.stem + "-" +
                              number(++p.report_id, 12) + ".zip");
    // Export only execution artifacts. Sidecars remain part of retention and
    // cleanup, but may contain a player's profile and never enter a report.
    std::vector<fs::path> retained;
    auto start = p.history.size() > p.opt.report_segments
                     ? p.history.size() - p.opt.report_segments
                     : 0;
    for (std::size_t i = start; i < p.history.size(); ++i) {
      retained.push_back(p.history[i].path);
      if (!p.opt.blob_segments)
        retained.emplace_back(utf8(p.history[i].path) + ".state");
    }
    auto cost = state.size() + image.size() + text.size() + 4096;
    if (!p.post(
            [&p, dest, state = std::move(state), image = std::move(image),
             text = std::move(text), retained = std::move(retained)] {
              auto temp = dest;
              temp += ".partial";
              try {
                Zip zip(temp);
                zip.put("report.txt", bytes(text));
                if (!state.empty())
                  zip.put("current.state", state);
                if (!image.empty())
                  zip.put("screenshot.ppm", image);
                if (!p.log_path.empty()) {
                  if (fs::is_symlink(p.log_path))
                    throw std::runtime_error("diagnostic log is a symlink");
                  zip.put("session.log", read(p.log_path));
                }
                for (const auto &file : retained) {
                  if (fs::is_symlink(file))
                    throw std::runtime_error("diagnostic history is a symlink");
                  zip.put("replays/" + file.filename().string(), read(file));
                }
                zip.close();
                fs::rename(temp, dest);
                prune_flat(p.opt.root, "bug-report-", ".zip", 2);
                std::lock_guard lock(p.message_mutex);
                p.message = "Saved bug report: " + dest.filename().string();
              } catch (...) {
                std::error_code ec;
                fs::remove(temp, ec);
                throw;
              }
            },
            cost, true))
      return {};
    return dest;
  } catch (const std::exception &x) {
    p.fail(x.what());
    return {};
  }
}
bool DiagnosticSession::recording() const {
  return impl_->active && impl_->writer &&
         impl_->writer->status() != Status::Io &&
         impl_->writer->status() != Status::Invalid;
}
void DiagnosticSession::stop(std::string_view reason) noexcept {
  impl_->active = false;
  try {
    impl_->fail(std::string(reason));
  } catch (...) {
  }
}
DiagnosticSession::Status DiagnosticSession::status() const {
  auto &p = *impl_;
  return !p.failure.empty() ? Status::Io
         : p.writer         ? p.writer->status()
                            : Status::Io;
}
const char *DiagnosticSession::error() const {
  auto &p = *impl_;
  return !p.failure.empty() ? p.failure.c_str()
         : p.writer         ? p.writer->error()
                            : "storage worker unavailable";
}
std::string DiagnosticSession::message() const {
  auto &p = *impl_;
  if (p.writer && (p.writer->status() == Status::Io ||
                   p.writer->status() == Status::Invalid))
    return std::string("Diagnostics stopped: ") + p.writer->error();
  std::lock_guard lock(p.message_mutex);
  return p.message;
}
std::size_t DiagnosticSession::free_bytes() const {
  return impl_->writer ? impl_->writer->free_bytes() : 0;
}
void DiagnosticSession::drain() {
  if (impl_->writer)
    impl_->writer->drain();
}
const char *DiagnosticSession::shutdown(const std::string &final_record) {
  auto &p = *impl_;
  if (!p.writer)
    return error();
  if (p.stream_open) {
    auto put = [&](const std::string &s) {
      for (;;) {
        if (p.writer->line(s))
          return true;
        if (p.writer->status() != Status::Full)
          return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };
    if (p.gap_marker) {
      put("G\n");
      p.gap_marker = false;
    }
    if (!final_record.empty())
      put(final_record);
    while (!p.writer->end() && p.writer->status() == Status::Full)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    p.stream_open = p.open = false;
  }
  p.active = false;
  return p.writer->shutdown();
}
} // namespace recomp::diagnostics
