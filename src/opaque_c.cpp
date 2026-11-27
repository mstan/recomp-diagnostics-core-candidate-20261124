/* SPDX-License-Identifier: MIT */
#include "recomp_diagnostics/opaque_writer.h"
#include "recomp_diagnostics/opaque_writer.hpp"
#include <cerrno>
#include <memory>
#include <stdexcept>
using namespace recomp::diagnostics;
struct rd_opaque_writer {
  OpaqueWriter writer;
  OpaqueProfile profile;
};
struct rd_history_admission {
  std::unique_ptr<OwnedHistoryAdmission> value;
};
namespace {
struct Record {
  void *value;
  rd_opaque_release release;
  Record(void *pointer, rd_opaque_release destroy)
      : value(pointer), release(destroy) {}
  Record(const Record &) = delete;
  Record &operator=(const Record &) = delete;
  ~Record() {
    if (release)
      release(value);
  }
};
std::string utf8(const std::filesystem::path &path) {
  const auto bytes = path.generic_u8string();
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
} // namespace
extern "C" {
rd_opaque_writer *rd_opaque_writer_create(int profile) {
  try {
    OpaqueProfile options;
    switch (profile) {
    case RD_OPAQUE_PSP:
      options = OpaqueProfile::psp();
      break;
    case RD_OPAQUE_DREAMCAST:
      options = OpaqueProfile::dreamcast();
      break;
    case RD_OPAQUE_PS2:
      options = OpaqueProfile::ps2();
      break;
    default:
      return nullptr;
    }
    return new rd_opaque_writer{{}, options};
  } catch (...) {
    return nullptr;
  }
}
int rd_opaque_writer_open(rd_opaque_writer *writer, const char *root,
                          const rd_opaque_stream *streams, size_t count,
                          uint64_t initial) {
  if (!writer || !root || !streams || !count || count > 32)
    return 0;
  try {
    std::vector<OpaqueStream> native;
    for (size_t i = 0; i < count; ++i) {
      const auto &input = streams[i];
      if (!input.name || (!input.header && input.header_bytes))
        return 0;
      OpaqueStream stream{input.name, {}, input.exclusive != 0};
      if (input.header_bytes) {
        const auto *bytes = static_cast<const std::uint8_t *>(input.header);
        stream.header.assign(bytes, bytes + input.header_bytes);
      }
      native.push_back(std::move(stream));
    }
    return writer->writer.open(std::filesystem::u8path(root), writer->profile,
                               native, initial);
  } catch (...) {
    writer->writer.invalidate();
    return 0;
  }
}
int rd_opaque_writer_push(rd_opaque_writer *writer, void *value,
                          rd_opaque_encode encode, rd_opaque_release release,
                          size_t queue_bytes, uint64_t bytes, uint64_t frames) {
  if (!writer || !encode) {
    if (release)
      release(value);
    return 0;
  }
  bool adopted = false;
  try {
    auto record = std::make_shared<Record>(value, release);
    adopted = true;
    return writer->writer.push(
        [record, encode](OpaqueSink &sink) {
          return encode(record->value,
                        reinterpret_cast<rd_opaque_sink *>(&sink)) != 0;
        },
        {queue_bytes, bytes, frames});
  } catch (...) {
    if (!adopted && release)
      release(value);
    writer->writer.invalidate();
    return 0;
  }
}
void rd_opaque_writer_close(rd_opaque_writer *writer) {
  if (writer)
    writer->writer.close();
}
void rd_opaque_writer_request_stop(rd_opaque_writer *writer) {
  if (writer)
    writer->writer.request_stop();
}
void rd_opaque_writer_join(rd_opaque_writer *writer) {
  if (writer)
    writer->writer.join();
}
void rd_opaque_writer_destroy(rd_opaque_writer *writer) { delete writer; }
int rd_opaque_writer_good(const rd_opaque_writer *writer) {
  return writer && writer->writer.good();
}
const char *rd_opaque_writer_error(const rd_opaque_writer *writer) {
  return writer ? writer->writer.error() : "cannot allocate diagnostic owner";
}
uint64_t rd_opaque_writer_written(const rd_opaque_writer *writer) {
  return writer ? writer->writer.stats().written_bytes : 0;
}
int rd_opaque_sink_write(rd_opaque_sink *sink, unsigned stream,
                         const void *bytes, size_t size) {
  if (!sink)
    return 0;
  try {
    return reinterpret_cast<OpaqueSink *>(sink)->write(stream, bytes, size);
  } catch (...) {
    return 0;
  }
}
int rd_opaque_sink_charge_auxiliary(rd_opaque_sink *sink, size_t size) {
  if (!sink)
    return 0;
  try {
    return reinterpret_cast<OpaqueSink *>(sink)->charge_auxiliary(size);
  } catch (...) {
    return 0;
  }
}
int rd_owned_history_reserve(const char *root, unsigned sessions,
                             unsigned depth, int protect,
                             rd_history_classify classify,
                             rd_history_allowed allowed, void *user,
                             int *os_error) {
  if (os_error)
    *os_error = 0;
  if (!root || !classify || !allowed)
    return -1;
  try {
    OwnedHistoryPolicy policy;
    policy.sessions = sessions;
    policy.directory_depth = depth;
    policy.protect_readonly = protect != 0;
    policy.classify = [&](const std::filesystem::path &path) {
      const auto name = utf8(path);
      const int result = classify(user, name.c_str());
      if (result < 0 || result > 2)
        throw std::runtime_error("native diagnostic owner/status parse failed");
      return static_cast<OwnedHistoryEntry>(result);
    };
    policy.allowed = [&](const std::filesystem::path &path, bool directory) {
      const auto name = utf8(path);
      const auto result = allowed(user, name.c_str(), directory);
      if (result < 0)
        throw std::runtime_error("native diagnostic filename policy failed");
      return result != 0;
    };
    return reserve_owned_history(std::filesystem::u8path(root), policy) ? 1 : 0;
  } catch (const std::filesystem::filesystem_error &error) {
    if (os_error)
      *os_error = error.code().value();
    return -1;
  } catch (...) {
    return -1;
  }
}
int rd_owned_history_remove(const char *path, unsigned depth, int protect,
                            rd_history_allowed allowed, void *user,
                            int *os_error) {
  if (os_error)
    *os_error = 0;
  if (!path || !allowed)
    return -1;
  try {
    OwnedHistoryPolicy policy;
    policy.directory_depth = depth;
    policy.protect_readonly = protect != 0;
    policy.allowed = [&](const std::filesystem::path &relative,
                         bool directory) {
      const auto name = utf8(relative);
      const auto result = allowed(user, name.c_str(), directory);
      if (result < 0)
        throw std::runtime_error("native diagnostic filename policy failed");
      return result != 0;
    };
    return remove_owned_history_entry(std::filesystem::u8path(path), policy)
               ? 1
               : 0;
  } catch (const std::filesystem::filesystem_error &error) {
    if (os_error)
      *os_error = error.code().value();
    return -1;
  } catch (...) {
    return -1;
  }
}
rd_history_admission *rd_history_admission_acquire(const char *path,
                                                   int *os_error) {
  if (os_error)
    *os_error = 0;
  if (!path)
    return nullptr;
  try {
    auto owner = OwnedHistoryAdmission::acquire(std::filesystem::u8path(path));
    if (!owner) {
#ifdef _WIN32
      if (os_error)
        *os_error = 183; // ERROR_ALREADY_EXISTS
#else
      if (os_error)
        *os_error = EEXIST;
#endif
      return nullptr;
    }
    return new rd_history_admission{std::move(owner)};
  } catch (const std::filesystem::filesystem_error &error) {
    if (os_error)
      *os_error = error.code().value();
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}
void rd_history_admission_release(rd_history_admission *owner) { delete owner; }
}
