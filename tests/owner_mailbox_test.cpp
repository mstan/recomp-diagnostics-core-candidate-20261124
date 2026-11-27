/* SPDX-License-Identifier: MIT */
#include "recomp_diagnostics/owner_mailbox.h"
#include "recomp_diagnostics/owner_mailbox.hpp"
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
using namespace recomp::diagnostics;
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::cerr << __LINE__ << ": " << #x << '\n';                             \
      std::abort();                                                            \
    }                                                                          \
  } while (0)
static void pending(OwnerMailbox &m) {
  while (!m.pending())
    std::this_thread::yield();
}
int main() {
  {
    OwnerMailbox m({3, 8});
    CHECK(m.call("long").status == MailboxStatus::request_too_large);
    auto producer =
        std::async(std::launch::async, [&] { return m.call("one"); });
    pending(m);
    CHECK(m.call("two").status == MailboxStatus::busy);
    const auto owner = std::this_thread::get_id();
    CHECK(m.poll([&](const std::string &command, MailboxReply &reply) {
      CHECK(std::this_thread::get_id() == owner && command == "one");
      CHECK(!m.poll([](const auto &, auto &) {}));
      CHECK(reply.append_line("first", 5));
      CHECK(!reply.append_line("oversized", 9));
      CHECK(reply.append_line("x", 1));
    }));
    auto result = producer.get();
    CHECK(result.status == MailboxStatus::completed);
    CHECK(result.overflow && result.data == "first\nx\n");
    CHECK(!m.pending());
  }
  {
    OwnerMailbox m(OwnerMailboxOptions::psx());
    auto producer =
        std::async(std::launch::async, [&] { return m.call("late", 500); });
    while (!m.pending()) {
      CHECK(producer.wait_for(std::chrono::milliseconds(0)) !=
            std::future_status::ready);
      std::this_thread::yield();
    }
    CHECK(m.poll([&](const auto &, MailboxReply &reply) {
      CHECK(producer.get().status == MailboxStatus::timed_out);
      CHECK(m.call("cannot replace active").status == MailboxStatus::busy);
      reply.assign("old response");
    }));
    auto next = std::async(std::launch::async, [&] { return m.call("next"); });
    pending(m);
    m.poll([](const auto &request, auto &reply) { reply.assign(request); });
    CHECK(next.get().data == "next");
  }
  {
    OwnerMailbox m(OwnerMailboxOptions::gba());
    auto producer =
        std::async(std::launch::async, [&] { return m.call("stop"); });
    pending(m);
    m.stop();
    CHECK(producer.get().status == MailboxStatus::stopped);
    CHECK(m.call("later").status == MailboxStatus::stopped);
    CHECK(!m.poll([](const auto &, auto &) {}));
  }
  {
    OwnerMailbox m(OwnerMailboxOptions::gba());
    const char *original = nullptr;
    auto producer = std::async(std::launch::async,
                               [&] { return m.call("large native capture"); });
    pending(m);
    m.poll([&](const auto &, auto &reply) {
      std::string data(64u * 1024u * 1024u + 128u, 'x');
      original = data.data();
      CHECK(reply.assign(std::move(data)));
    });
    auto result = producer.get();
    CHECK(result.status == MailboxStatus::completed && !result.overflow);
    CHECK(result.data.size() == 64u * 1024u * 1024u + 128u &&
          result.data.data() == original);
  }
  {
    auto *m = rd_owner_mailbox_create_psx();
    CHECK(m);
    auto producer = std::async(std::launch::async, [&] {
      return rd_owner_mailbox_call(m, "c facade", 0);
    });
    auto handler = [](void *, const char *request, rd_mailbox_reply *reply) {
      CHECK(rd_mailbox_reply_line(reply, request));
      CHECK(!rd_mailbox_reply_overflow(reply));
    };
    while (!rd_owner_mailbox_poll(m, handler, nullptr))
      std::this_thread::yield();
    auto *result = producer.get();
    CHECK(rd_mailbox_result_status(result) == RD_MAILBOX_COMPLETED);
    CHECK(std::string(rd_mailbox_result_data(result),
                      rd_mailbox_result_size(result)) == "c facade\n");
    rd_mailbox_result_destroy(result);
    rd_owner_mailbox_stop(m);
    rd_owner_mailbox_destroy(m);
  }
  std::cout << "owner thread, cancellation, bounds, C facade and native reply "
               "ownership passed\n";
}
