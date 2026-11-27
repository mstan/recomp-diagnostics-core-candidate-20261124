/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace recomp::diagnostics {
struct OpaqueProfile {
  std::size_t queue_bytes = 0, queue_rows = 0;
  bool release_bytes_on_dequeue = false;
  bool stop_on_encoder_failure = false;
  std::uint64_t admitted_bytes = 0, admitted_frames = 0;
  std::uint64_t write_bytes = 0, attachment_bytes = 0;
  std::uint64_t file_bytes = 0, session_bytes = 0, final_reserve = 0;
  unsigned retained_sessions = 0, poll_ms = 100, flush_ms = 100,
           buffer_bytes = 0;
  static OpaqueProfile psp();
  static OpaqueProfile dreamcast();
  static OpaqueProfile ps2();
};
struct OpaqueStream {
  std::string name;
  std::vector<std::uint8_t> header;
  bool exclusive = false;
};
struct OpaqueCharge {
  std::size_t queue_bytes = 0;
  std::uint64_t admitted_bytes = 0, frames = 0;
};
struct OpaqueStats {
  std::uint64_t admitted_bytes = 0, admitted_frames = 0;
  std::uint64_t written_bytes = 0, attachment_bytes = 0, completed = 0;
};
class OpaqueWriter;
class OpaqueSink {
public:
  bool write(unsigned stream, const void *bytes, std::size_t size);
  /* Optional native codec artifacts share accounting but choose their own
   * failure policy. A declined reservation does not poison the main streams. */
  bool charge_auxiliary(std::size_t size);

private:
  friend class OpaqueWriter;
  explicit OpaqueSink(void *state) : state_(state) {}
  void *state_;
};
/* Exactly one worker/queue replaces the caller's recorder worker. The producer
 * passes owned immutable bytes/codec state; encoding and all stream I/O run on
 * this worker. No guest APIs may be captured. close stops admission and drains
 * accepted work; it has no elapsed cutoff. Stop producers before open/close.
 * Profile charges intentionally preserve legacy logical queue accounting,
 * including whether an in-flight row still occupies its byte reservation. */
class OpaqueWriter {
public:
  using Encoder = std::function<bool(OpaqueSink &)>;
  OpaqueWriter();
  ~OpaqueWriter();
  OpaqueWriter(const OpaqueWriter &) = delete;
  OpaqueWriter &operator=(const OpaqueWriter &) = delete;
  bool open(const std::filesystem::path &directory, OpaqueProfile profile,
            const std::vector<OpaqueStream> &streams,
            std::uint64_t initial_bytes = 0);
  bool push(Encoder encoder, OpaqueCharge charge);
  void request_stop();
  /* For adapters whose last sender signals EOF. Exactly one control owner
   * joins after all producers release their sender handles. */
  void join();
  void close();
  bool good() const;
  bool is_open() const;
  const char *error() const;
  void invalidate();
  OpaqueStats stats() const;
  /* Cold setup/final capture only. Filename/contents/export eligibility remain
   * adapter policy. This separate partition never consumes stream admission. */
  bool attach(const std::string &name, const void *bytes, std::size_t size);

private:
  friend class OpaqueSink;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
/* Codec manifests can validate private artifacts at their existing observation
 * point without moving their format, hash, native seeds or export allowlist. */
class ArtifactBudget {
public:
  ArtifactBudget(std::uint64_t file_limit, std::uint64_t total_limit)
      : file_limit_(file_limit), total_limit_(total_limit) {}
  bool charge(std::uint64_t bytes);
  std::uint64_t used() const { return used_; }

private:
  std::uint64_t file_limit_, total_limit_, used_ = 0;
};
enum class OwnedHistoryEntry { unowned, protected_entry, reclaimable };
struct OwnedHistoryPolicy {
  unsigned sessions = 0, directory_depth = 0;
  std::string prefix = "session-";
  bool protect_readonly = true;
  /* Parse existing owner/status formats; never delete in these callbacks. */
  std::function<OwnedHistoryEntry(const std::filesystem::path &)> classify;
  std::function<bool(const std::filesystem::path &, bool directory)> allowed;
};
/* Reserve before creating a new session. All recognized owned sessions count;
 * only complete, fully allowlisted, link-free trees may be reclaimed. */
bool reserve_owned_history(const std::filesystem::path &root,
                           const OwnedHistoryPolicy &policy);
bool remove_owned_history_entry(const std::filesystem::path &path,
                                const OwnedHistoryPolicy &policy);
class OwnedHistoryAdmission {
public:
  static std::unique_ptr<OwnedHistoryAdmission>
  acquire(const std::filesystem::path &path);
  ~OwnedHistoryAdmission();
  OwnedHistoryAdmission(const OwnedHistoryAdmission &) = delete;

private:
  explicit OwnedHistoryAdmission(std::filesystem::path path)
      : path_(std::move(path)) {}
  std::filesystem::path path_;
  bool held_ = false;
};
bool plain_owned_path(const std::filesystem::path &path);
unsigned recorder_process_id();
bool recorder_process_alive(unsigned pid);
} // namespace recomp::diagnostics
