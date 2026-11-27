/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace recomp::diagnostics {
struct OwnerMailboxOptions {
  std::size_t request_bytes;
  std::size_t reply_bytes;
  static OwnerMailboxOptions psx();
  static OwnerMailboxOptions gba();
};
enum class MailboxStatus {
  completed,
  stopped,
  busy,
  request_too_large,
  timed_out,
  failed
};
struct MailboxResult {
  MailboxStatus status = MailboxStatus::failed;
  std::string data;
  bool overflow = false;
};
class MailboxReply {
public:
  explicit MailboxReply(std::size_t limit) : limit_(limit) {}
  bool append_line(const char *line, std::size_t length);
  bool assign(std::string value);
  bool overflow() const { return overflow_; }

private:
  friend class OwnerMailbox;
  std::size_t limit_;
  bool overflow_ = false;
  std::string data_;
};
/* One owned in-flight request. No worker: poll executes the handler on the
 * engine's existing owner thread, outside the synchronization lock. Producer
 * calls wait for that request's result. A transport may expire its own wait;
 * late results are discarded and can never answer a subsequent request.
 * stop wakes producers; join external callers before destroying the mailbox.
 * poll is serialized on one owner and rejects reentrant/concurrent polls. */
class OwnerMailbox {
public:
  using Handler = std::function<void(const std::string &, MailboxReply &)>;
  explicit OwnerMailbox(OwnerMailboxOptions options);
  ~OwnerMailbox();
  OwnerMailbox(const OwnerMailbox &) = delete;
  OwnerMailbox &operator=(const OwnerMailbox &) = delete;
  MailboxResult call(std::string request, std::uint32_t transport_wait_ms = 0);
  bool poll(const Handler &handler);
  void stop();
  bool pending() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace recomp::diagnostics
