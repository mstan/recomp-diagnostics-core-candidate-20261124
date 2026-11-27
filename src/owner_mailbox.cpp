/* SPDX-License-Identifier: MIT */
#include "recomp_diagnostics/owner_mailbox.hpp"
#include "recomp_diagnostics/owner_mailbox.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

namespace recomp::diagnostics {
OwnerMailboxOptions OwnerMailboxOptions::psx() {
  return {8191, 64u * 1024u * 1024u - 1u};
}
OwnerMailboxOptions OwnerMailboxOptions::gba() {
  return {262144u + 4096u - 1u, std::string{}.max_size()};
}
bool MailboxReply::append_line(const char *line, std::size_t length) {
  if (!line || length >= limit_ - data_.size()) {
    overflow_ = true;
    return false;
  }
  try {
    /* Reserve before committing either part: a dropped line leaves every
     * preceding line intact and a later smaller line can still fit. */
    data_.reserve(data_.size() + length + 1);
    data_.append(line, length);
    data_.push_back('\n');
    return true;
  } catch (...) {
    overflow_ = true;
    return false;
  }
}
bool MailboxReply::assign(std::string value) {
  if (value.size() > limit_) {
    overflow_ = true;
    return false;
  }
  data_ = std::move(value);
  return true;
}
struct OwnerMailbox::Impl {
  struct Request {
    std::string command;
    MailboxResult result;
    bool done = false, canceled = false, working = false;
  };
  explicit Impl(OwnerMailboxOptions value) : options(value) {}
  OwnerMailboxOptions options;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<Request> request;
  bool stopped = false, polling = false;
};
OwnerMailbox::OwnerMailbox(OwnerMailboxOptions options)
    : impl_(new Impl(options)) {}
OwnerMailbox::~OwnerMailbox() { stop(); }
MailboxResult OwnerMailbox::call(std::string command, std::uint32_t wait_ms) {
  auto &p = *impl_;
  std::unique_lock<std::mutex> lock(p.mutex);
  if (p.stopped)
    return {MailboxStatus::stopped, {}, false};
  if (command.size() > p.options.request_bytes)
    return {MailboxStatus::request_too_large, {}, false};
  if (p.request)
    return {MailboxStatus::busy, {}, false};
  auto request = std::make_shared<Impl::Request>();
  request->command = std::move(command);
  p.request = request;
  auto ready = [&] { return request->done || p.stopped; };
  bool completed = true;
  if (wait_ms)
    completed =
        p.changed.wait_for(lock, std::chrono::milliseconds(wait_ms), ready);
  else
    p.changed.wait(lock, ready);
  if (!completed) {
    request->canceled = true;
    if (!request->working && p.request == request)
      p.request.reset();
    return {MailboxStatus::timed_out, {}, false};
  }
  if (p.stopped)
    return {MailboxStatus::stopped, {}, false};
  return std::move(request->result);
}
bool OwnerMailbox::poll(const Handler &handler) {
  auto &p = *impl_;
  std::shared_ptr<Impl::Request> request;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (p.stopped || p.polling || !p.request)
      return false;
    request = p.request;
    request->working = true;
    p.polling = true;
  }
  MailboxReply reply(p.options.reply_bytes);
  MailboxStatus status = MailboxStatus::completed;
  try {
    handler(request->command, reply);
  } catch (...) {
    status = MailboxStatus::failed;
  }
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    if (!request->canceled && !p.stopped) {
      request->result = {status, std::move(reply.data_), reply.overflow_};
      request->done = true;
    }
    if (p.request == request)
      p.request.reset();
    p.polling = false;
    p.changed.notify_all();
  }
  return true;
}
void OwnerMailbox::stop() {
  auto &p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  p.stopped = true;
  p.changed.notify_all();
}
bool OwnerMailbox::pending() const {
  auto &p = *impl_;
  std::lock_guard<std::mutex> lock(p.mutex);
  return !p.stopped && bool(p.request);
}
} // namespace recomp::diagnostics

using recomp::diagnostics::MailboxReply;
using recomp::diagnostics::MailboxResult;
using recomp::diagnostics::OwnerMailbox;
struct rd_owner_mailbox {
  OwnerMailbox value{recomp::diagnostics::OwnerMailboxOptions::psx()};
};
struct rd_mailbox_result {
  MailboxResult value;
};
extern "C" {
rd_owner_mailbox *rd_owner_mailbox_create_psx(void) {
  try {
    return new rd_owner_mailbox;
  } catch (...) {
    return nullptr;
  }
}
void rd_owner_mailbox_stop(rd_owner_mailbox *m) {
  if (m)
    m->value.stop();
}
void rd_owner_mailbox_destroy(rd_owner_mailbox *m) { delete m; }
rd_mailbox_result *rd_owner_mailbox_call(rd_owner_mailbox *m,
                                         const char *request,
                                         uint32_t wait_ms) {
  if (!m || !request)
    return nullptr;
  try {
    return new rd_mailbox_result{m->value.call(request, wait_ms)};
  } catch (...) {
    return nullptr;
  }
}
int rd_owner_mailbox_poll(rd_owner_mailbox *m, rd_mailbox_handler handler,
                          void *user) {
  if (!m || !handler)
    return 0;
  try {
    return m->value.poll([&](const std::string &request, MailboxReply &reply) {
      handler(user, request.c_str(),
              reinterpret_cast<rd_mailbox_reply *>(&reply));
    });
  } catch (...) {
    return 0;
  }
}
int rd_mailbox_reply_line(rd_mailbox_reply *r, const char *line) {
  return r && line &&
         reinterpret_cast<MailboxReply *>(r)->append_line(line,
                                                          std::strlen(line));
}
int rd_mailbox_reply_overflow(const rd_mailbox_reply *r) {
  return r && reinterpret_cast<const MailboxReply *>(r)->overflow();
}
int rd_mailbox_result_status(const rd_mailbox_result *r) {
  return r ? static_cast<int>(r->value.status) : RD_MAILBOX_FAILED;
}
const char *rd_mailbox_result_data(const rd_mailbox_result *r) {
  return r ? r->value.data.c_str() : "";
}
size_t rd_mailbox_result_size(const rd_mailbox_result *r) {
  return r ? r->value.data.size() : 0;
}
void rd_mailbox_result_destroy(rd_mailbox_result *r) { delete r; }
}
