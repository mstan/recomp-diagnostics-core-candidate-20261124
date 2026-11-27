// replay_writer.cpp â€” see replay_writer.h.

#include "replay_writer.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <thread>

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define RECOMP_DIAGNOSTICS_REPLAY_WRITER_THREADS 0
#else
#define RECOMP_DIAGNOSTICS_REPLAY_WRITER_THREADS 1
#endif

namespace recomp::diagnostics {

namespace {

constexpr std::uint64_t kP1 = 11400714785074694791ull;
constexpr std::uint64_t kP2 = 14029467366897019727ull;
constexpr std::uint64_t kP3 = 1609587929392839161ull;

inline std::uint64_t rotl(std::uint64_t v, unsigned r) {
    return (v << r) | (v >> (64u - r));
}
inline std::uint64_t mix(std::uint64_t acc, std::uint64_t lane) {
    acc += lane * kP2;
    acc = rotl(acc, 31);
    return acc * kP1;
}
inline std::uint64_t load64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;  // GBA hosts are little-endian; the hash is host-local anyway.
}

enum Op : std::uint32_t { BEGIN = 1, SNAPSHOT, LINE, FRAME, REMOVE, END, SNAPSHOT_RAW, TASK };
enum StatusCode : int { OK = 0, FULL = 1, INVALID = 2, IO = 3 };

struct Command {
    std::uint32_t op = 0;
    std::uint32_t count = 0;   // FRAME parts
    std::uint64_t size = 0;    // payload bytes following this header
    std::uint64_t frame = 0;
    std::uint32_t kind = 0;    // SNAPSHOT kind character
    std::uint64_t sizes[ReplayWriter::kMaxParts] = {};
    std::uint32_t reuse_mask = 0;
};

constexpr std::size_t kFileBuffer = 64u * 1024u;

// Recorder paths are UTF-8. Keep the native wide path on Windows rather than
// losing non-ASCII player/profile names through the active ANSI code page.
static std::FILE* file_open(const char* path, const char* mode) {
#ifdef _WIN32
    try {
        const auto native = std::filesystem::u8path(path);
        const std::wstring wide_mode(mode, mode + std::strlen(mode));
        return _wfopen(native.c_str(), wide_mode.c_str());
    } catch (...) { errno = EINVAL; return nullptr; }
#else
    return std::fopen(path, mode);
#endif
}
static int file_remove(const char* path) {
#ifdef _WIN32
    try { return _wremove(std::filesystem::u8path(path).c_str()); }
    catch (...) { errno = EINVAL; return -1; }
#else
    return std::remove(path);
#endif
}
static int file_rename(const char* source, const char* dest) {
#ifdef _WIN32
    try { return _wrename(std::filesystem::u8path(source).c_str(),
                         std::filesystem::u8path(dest).c_str()); }
    catch (...) { errno = EINVAL; return -1; }
#else
    return std::rename(source, dest);
#endif
}

}  // namespace

void ReplayHash::update(const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    length += size;
    if (tail_len) {
        while (tail_len < 8 && size) { tail[tail_len++] = *p++; --size; }
        if (tail_len < 8) return;
        state = mix(state, load64(tail));
        tail_len = 0;
    }
    while (size >= 8) {
        state = mix(state, load64(p));
        p += 8;
        size -= 8;
    }
    while (size) { tail[tail_len++] = *p++; --size; }
}

std::uint64_t ReplayHash::finish() const {
    std::uint64_t h = state ^ (length * kP1);
    for (unsigned i = 0; i < tail_len; ++i)
        h = rotl(h ^ (tail[i] * kP3), 11) * kP1;
    h ^= h >> 33; h *= kP2;
    h ^= h >> 29; h *= kP3;
    h ^= h >> 32;
    return h;
}

std::uint64_t replay_hash(const void* data, std::size_t size) {
    ReplayHash h;
    h.update(data, size);
    return h.finish();
}

static bool rotate_history(const char *base) {
  const char *suffix[] = {"", ".sav", ".config.ini", ".mods.toml", ".mods.txt"};
  char src[1500], dst[1500], row[64];
  for (int i = 2; i >= 0; --i) {
    if (i)
      snprintf(src, sizeof src, "%s.previous-%d", base, i);
    else
      snprintf(src, sizeof src, "%s", base);
    FILE *f = file_open(src, "rb");
    if (!f) {
      if (errno != ENOENT)
        return false;
      continue;
    }
    bool owned = fgets(row, sizeof row, f) && !strcmp(row, "GENESISREPLAY 1\n");
    fclose(f);
    if (!owned)
      return false;
    for (unsigned j = 0; j < sizeof suffix / sizeof suffix[0]; ++j) {
      if (i)
        snprintf(src, sizeof src, "%s.previous-%d%s", base, i, suffix[j]);
      else
        snprintf(src, sizeof src, "%s%s", base, suffix[j]);
      if (i == 2) {
        if (file_remove(src) && errno != ENOENT)
          return false;
      } else {
        snprintf(dst, sizeof dst, "%s.previous-%d%s", base, i + 1, suffix[j]);
        if (file_rename(src, dst) && errno != ENOENT)
          return false;
      }
    }
  }
  return true;
}

struct ReplayWriter::Impl {
    Format format;
    const std::size_t queue_bytes;
    std::vector<std::uint8_t> ring;
    explicit Impl(Format f, std::size_t capacity):format(f),
        queue_bytes(capacity ? capacity : f==Format::Hashed64 ? ReplayWriter::kQueueBytes : 16u*1024u*1024u),ring(queue_bytes) {}
    std::atomic<std::size_t> read_pos{0};
    std::atomic<std::size_t> write_pos{0};
    std::atomic<int> error{OK};
    std::atomic<std::size_t> pending_need{0};
    bool session = false; // producer only; begin/end order is validated before enqueue
    std::atomic<bool> stop{false};
    std::size_t head = 0;  // producer only
#if RECOMP_DIAGNOSTICS_REPLAY_WRITER_THREADS
    std::thread thread;
#endif
    bool joined = false;

    // â”€â”€ producer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    std::size_t free_space() const {
        const std::size_t tail = read_pos.load(std::memory_order_acquire);
        return (tail + queue_bytes - head - 1) % queue_bytes;
    }
    void put(std::size_t at, const void* data, std::size_t n) {
        if (!n) return;
        const std::size_t first = std::min(n, queue_bytes - at);
        std::memcpy(ring.data() + at, data, first);
        if (n > first)
            std::memcpy(ring.data(),
                        static_cast<const std::uint8_t*>(data) + first, n - first);
    }
    void fail(int code) {
        int expected = OK;
        error.compare_exchange_strong(expected, code);
    }
    bool enqueue(const Command& c, const void* const* data,
                 const std::size_t* sizes, unsigned parts) {
        if (stop.load(std::memory_order_relaxed) ||
            error.load(std::memory_order_relaxed) != OK)
            return false;
        const std::size_t max_payload = queue_bytes - sizeof(Command) - 1;
        if (c.size > max_payload || (parts && (!data || !sizes))) { fail(INVALID); return false; }
        std::size_t total = 0;
        for (unsigned i=0; i<parts; ++i) {
            if ((sizes[i] && !data[i]) || sizes[i] > c.size-total) { fail(INVALID); return false; }
            total += sizes[i];
        }
        if (total != c.size) { fail(INVALID); return false; }
        const std::size_t need = sizeof(Command) + static_cast<std::size_t>(c.size);
        if (need > free_space()) {
            pending_need.store(need, std::memory_order_relaxed);
            return false;
        }
        std::size_t at = head;
        put(at, &c, sizeof(c));
        at = (at + sizeof(c)) % queue_bytes;
        for (unsigned i = 0; i < parts; ++i) {
            if (sizes[i] && !data[i]) { fail(INVALID); return false; }
            put(at, data[i], sizes[i]);
            at = (at + sizes[i]) % queue_bytes;
        }
        head = at;
        pending_need.store(0, std::memory_order_relaxed);
        // Release publishes every payload byte before the worker sees head.
        write_pos.store(at, std::memory_order_release);
        return true;
    }

    // â”€â”€ worker â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    void get(std::size_t at, void* data, std::size_t n) const {
        if (!n) return;
        const std::size_t first = std::min(n, queue_bytes - at);
        std::memcpy(data, ring.data() + at, first);
        if (n > first)
            std::memcpy(static_cast<std::uint8_t*>(data) + first, ring.data(),
                        n - first);
    }
    void hash_ring(ReplayHash& h, std::size_t at, std::size_t n) const {
        const std::size_t first = std::min(n, queue_bytes - at);
        h.update(ring.data() + at, first);
        if (n > first) h.update(ring.data(), n - first);
    }
    std::uint32_t hash32_ring(std::size_t at, std::size_t n) const {
        std::uint32_t hash = 2166136261u;
        while (n) {
            const std::size_t part = std::min(n, queue_bytes - at);
            for (std::size_t i = 0; i < part; ++i)
                hash = (hash ^ ring[at + i]) * 16777619u;
            n -= part;
            at = 0;
        }
        return hash;
    }
    bool write_ring(std::FILE* f, std::size_t at, std::size_t n) const {
        if (!f) return false;
        const std::size_t first = std::min(n, queue_bytes - at);
        if (first && std::fwrite(ring.data() + at, 1, first, f) != first)
            return false;
        return n == first ||
               std::fwrite(ring.data(), 1, n - first, f) == n - first;
    }
    static std::string take_string(const std::vector<std::uint8_t>& blob,
                                   std::size_t& at, bool& ok) {
        std::uint32_t n = 0;
        if (at + 4 > blob.size()) { ok = false; return {}; }
        std::memcpy(&n, blob.data() + at, 4);
        at += 4;
        if (at + n > blob.size()) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(blob.data() + at), n);
        at += n;
        return s;
    }
    static bool copy_optional(const std::string& source, const std::string& dest) {
        if (source.empty()) return true;
        std::FILE* in = file_open(source.c_str(), "rb");
        if (!in) return errno == ENOENT;
        std::FILE* out = file_open(dest.c_str(), "wb");
        bool ok = out != nullptr;
        char bytes[8192];
        std::size_t n;
        while (ok && (n = std::fread(bytes, 1, sizeof(bytes), in)) != 0)
            ok = std::fwrite(bytes, 1, n, out) == n;
        if (std::ferror(in)) ok = false;
        if (std::fclose(in)) ok = false;
        if (out && std::fclose(out)) ok = false;
        return ok;
    }
    static bool close_files(std::FILE*& text, std::FILE*& states) {
        bool ok = true;
        if (states && std::fclose(states)) ok = false;
        if (text && std::fclose(text)) ok = false;
        text = states = nullptr;
        return ok;
    }

    void run() {
        std::FILE* text = nullptr;
        std::FILE* states = nullptr;
        std::vector<char> text_buffer(kFileBuffer), state_buffer(kFileBuffer);
        std::size_t tail = 0;
        std::uint64_t offset = 0;
        std::uint64_t last_hash[ReplayWriter::kMaxParts] = {};
        auto last_flush = std::chrono::steady_clock::now();
        bool dirty = false;
        // Test-only slow storage (acceptance of back-pressure recovery):
        // RECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US delays every frame record.
        long frame_delay_us = 0;
        if (const char* e = std::getenv("RECOMP_DIAGNOSTICS_TEST_WRITER_DELAY_US"))
            frame_delay_us = std::strtol(e, nullptr, 10);
        // Deterministic blocked-sink fixture for raw-row recorders, which do
        // not enqueue FRAME operations. Unset in normal recording.
        if (const char* e = std::getenv("RECOMP_DIAGNOSTICS_TEST_WRITER_START_DELAY_US")) {
            const long delay = std::strtol(e, nullptr, 10);
            if (delay > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay));
        }
        for (;;) {
            if (write_pos.load(std::memory_order_acquire) != tail) {
                Command c;
                get(tail, &c, sizeof(c));
                std::size_t at = (tail + sizeof(c)) % queue_bytes;
                bool ok = true;
                switch (c.op) {
                case BEGIN: {
                    std::vector<std::uint8_t> blob(static_cast<std::size_t>(c.size));
                    get(at, blob.data(), blob.size());
                    std::size_t cursor = 0;
                    const std::string path = take_string(blob, cursor, ok);
                    const std::string header = take_string(blob, cursor, ok);
                    ok = close_files(text, states) && ok;
                    offset = 0;
                    std::memset(last_hash, 0, sizeof(last_hash));
                    if (!ok) break;
                    if(format==Format::Genesis32 && !rotate_history(path.c_str())) {ok=false;break;}
                    text = file_open(path.c_str(), "wb");
                    states = file_open((path + (format==Format::Hashed64?".state":".sav")).c_str(), "wb");
                    ok = text && states &&
                         !std::setvbuf(text, text_buffer.data(), _IOFBF, kFileBuffer) &&
                         !std::setvbuf(states, state_buffer.data(), _IOFBF, kFileBuffer) &&
                         std::fputs(header.c_str(), text) != EOF;
                    std::uint32_t copies = 0, inlines = 0;
                    if (ok && cursor + 8 <= blob.size()) {
                        std::memcpy(&copies, blob.data() + cursor, 4);
                        std::memcpy(&inlines, blob.data() + cursor + 4, 4);
                        cursor += 8;
                    } else {
                        ok = false;
                    }
                    for (std::uint32_t i = 0; ok && i < copies; ++i) {
                        const std::string src = take_string(blob, cursor, ok);
                        const std::string dst = take_string(blob, cursor, ok);
                        ok = ok && copy_optional(src, dst);
                    }
                    for (std::uint32_t i = 0; ok && i < inlines; ++i) {
                        const std::string body = take_string(blob, cursor, ok);
                        const std::string dst = take_string(blob, cursor, ok);
                        std::FILE* f = ok ? file_open(dst.c_str(), "wb") : nullptr;
                        ok = f && std::fwrite(body.data(), 1, body.size(), f) == body.size();
                        if (f && std::fclose(f)) ok = false;
                    }
                    break;
                }
                case SNAPSHOT: {
                    ok = write_ring(states, at, static_cast<std::size_t>(c.size));
                    if(format==Format::Genesis32) {
                        uint32_t hash=2166136261u;
                        for(size_t i=0;i<c.size;++i)hash=(hash^ring[(at+i)%queue_bytes])*16777619u;
                        ok=ok&&text&&std::fprintf(text,"S %llu %llu %08x\n",
                            (unsigned long long)offset,(unsigned long long)c.size,hash)>=0;
                    } else if(format==Format::Snes32) {
                        ok=ok&&text&&std::fprintf(text,"S %llu %llu %08x\n",
                            (unsigned long long)offset,(unsigned long long)c.size,
                            hash32_ring(at,static_cast<std::size_t>(c.size)))>=0;
                    } else {
                        ReplayHash h;hash_ring(h,at,static_cast<std::size_t>(c.size));
                        ok=ok&&text&&std::fprintf(text,"S %llu %llu %016llx %c\n",
                            (unsigned long long)offset,(unsigned long long)c.size,
                            (unsigned long long)h.finish(),static_cast<char>(c.kind))>=0;
                    }
                    offset += c.size;
                    break;
                }
                case SNAPSHOT_RAW:
                    ok = write_ring(states, at, static_cast<std::size_t>(c.size));
                    offset += c.size;
                    break;
                case LINE:
                    ok = write_ring(text, at, static_cast<std::size_t>(c.size));
                    break;
                case FRAME: {
                    if (frame_delay_us > 0)
                        std::this_thread::sleep_for(std::chrono::microseconds(frame_delay_us));
                    ok = text && std::fprintf(text, "H %llu",
                        static_cast<unsigned long long>(c.frame)) >= 0;
                    for (std::uint32_t i = 0; ok && i < c.count; ++i) {
                        if (!(c.reuse_mask & (1u << i))) {
                            if (format == Format::Snes32) {
                                last_hash[i] = hash32_ring(at, static_cast<std::size_t>(c.sizes[i]));
                            } else {
                                ReplayHash h;
                                hash_ring(h, at, static_cast<std::size_t>(c.sizes[i]));
                                last_hash[i] = h.finish();
                            }
                            at = (at + c.sizes[i]) % queue_bytes;
                        }
                        ok = format == Format::Snes32
                            ? std::fprintf(text, " %08x",static_cast<unsigned>(last_hash[i])) >= 0
                            : std::fprintf(text, " %016llx",static_cast<unsigned long long>(last_hash[i])) >= 0;
                    }
                    ok = ok && std::fputc('\n', text) != EOF;
                    break;
                }
                case REMOVE: {
                    std::string path(static_cast<std::size_t>(c.size), '\0');
                    get(at, path.data(), path.size());
                    ok = file_remove(path.c_str()) == 0 || errno == ENOENT;
                    break;
                }
                case END:
                    ok = close_files(text, states);
                    break;
                case TASK: {
                    std::function<void()>* task = nullptr;
                    get(at, &task, sizeof task);
                    std::unique_ptr<std::function<void()>> owned(task);
                    // Reports observe all accepted records ahead of them,
                    // including buffered checkpoint bytes and their text rows.
                    try {
                        if ((states && std::fflush(states)) || (text && std::fflush(text))) ok = false;
                        else (*owned)();
                    } catch (...) { ok = false; }
                    break;
                }
                default:
                    ok = false;
                    break;
                }
                if (!ok) {
                    fail(IO);
                    // Release this record before discarding remaining work.
                    tail = (tail + sizeof(c) + static_cast<std::size_t>(c.size)) % queue_bytes;
                    read_pos.store(tail, std::memory_order_release);
                    break;
                }
                // The payload stays owned until disk I/O and hashing finish;
                // the producer refuses work at capacity rather than waiting.
                tail = (tail + sizeof(c) + static_cast<std::size_t>(c.size)) % queue_bytes;
                read_pos.store(tail, std::memory_order_release);
                dirty = text != nullptr;
            } else {
                if (stop.load(std::memory_order_acquire)) {
                    if (write_pos.load(std::memory_order_acquire) == tail) break;
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto now = std::chrono::steady_clock::now();
            if (dirty && now - last_flush >= std::chrono::milliseconds(100)) {
                // Checkpoints reach the OS before the records that cite them.
                if ((states && std::fflush(states)) || (text && std::fflush(text))) {
                    fail(IO);
                    break;
                }
                last_flush = now;
                dirty = false;
            }
        }
        // A failed sink does not execute later jobs, but still releases every
        // immutable capture. The producer stops enqueueing after a sticky error.
        while (tail != write_pos.load(std::memory_order_acquire)) {
            Command c; get(tail, &c, sizeof c);
            if (c.op == TASK) {
                std::function<void()>* task = nullptr;
                get((tail + sizeof c) % queue_bytes, &task, sizeof task);
                delete task;
            }
            tail = (tail + sizeof c + static_cast<std::size_t>(c.size)) % queue_bytes;
            read_pos.store(tail, std::memory_order_release);
        }
        if (!close_files(text, states)) fail(IO);
    }
};

ReplayWriter::ReplayWriter(Format f, std::size_t capacity) : impl_(new Impl(f,capacity)) {}

std::unique_ptr<ReplayWriter> ReplayWriter::create(Format format) {
    return create_with_capacity(format,0);
}

std::unique_ptr<ReplayWriter> ReplayWriter::create_with_capacity(Format format, std::size_t capacity) {
#if RECOMP_DIAGNOSTICS_REPLAY_WRITER_THREADS
    if (capacity && (capacity <= sizeof(Command) || capacity > kQueueBytes)) return nullptr;
    std::unique_ptr<ReplayWriter> w(new ReplayWriter(format,capacity));
    try {
        Impl* impl = w->impl_.get();
        impl->thread = std::thread([impl] { impl->run(); });
    } catch (const std::system_error&) {
        return nullptr;
    }
    return w;
#else
    return nullptr;
#endif
}

ReplayWriter::~ReplayWriter() { shutdown(); }

bool ReplayWriter::post(std::function<void()> work, std::size_t retained_bytes) {
    auto& p = *impl_;
    if (!work || retained_bytes > p.queue_bytes - sizeof(Command) - sizeof(void*) - 1) {
        p.pending_need.store(p.queue_bytes, std::memory_order_relaxed);
        return false;
    }
    const std::size_t payload = sizeof(void*) + retained_bytes;
    const std::size_t need = sizeof(Command) + payload;
    if (p.stop.load(std::memory_order_relaxed) || p.error.load(std::memory_order_relaxed) != OK)
        return false;
    if (need > p.free_space()) {
        p.pending_need.store(need, std::memory_order_relaxed);
        return false;
    }
    auto owned = std::make_unique<std::function<void()>>(std::move(work));
    Command c; c.op = TASK; c.size = payload;
    p.put(p.head, &c, sizeof c);
    auto* task = owned.get();
    p.put((p.head + sizeof c) % p.queue_bytes, &task, sizeof task);
    p.head = (p.head + need) % p.queue_bytes;
    owned.release();
    p.pending_need.store(0, std::memory_order_relaxed);
    p.write_pos.store(p.head, std::memory_order_release);
    return true;
}

void ReplayWriter::drain() {
    auto& p = *impl_;
    while (p.read_pos.load(std::memory_order_acquire) != p.write_pos.load(std::memory_order_acquire) &&
           p.error.load(std::memory_order_relaxed) == OK)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

namespace {
void append_string(std::vector<std::uint8_t>& blob, const std::string& s) {
    const auto n = static_cast<std::uint32_t>(s.size());
    const auto at = blob.size();
    blob.resize(at + 4 + s.size());
    std::memcpy(blob.data() + at, &n, 4);
    if (!s.empty()) std::memcpy(blob.data() + at + 4, s.data(), s.size());
}
}  // namespace

bool ReplayWriter::begin(const std::string& path, const std::string& header,
                         const std::vector<Sidecar>& sidecars,
                         const std::vector<Sidecar>& inline_texts) {
    const std::size_t max_payload = impl_->queue_bytes - sizeof(Command) - 1;
    std::size_t total = 8;
    auto fits = [&](const std::string& value) {
        if (total > max_payload-4 || value.size() > max_payload-total-4) return false;
        total += 4 + value.size(); return true;
    };
    if (sidecars.size()>UINT32_MAX || inline_texts.size()>UINT32_MAX || !fits(path) || !fits(header)) {
        impl_->fail(INVALID); return false;
    }
    for (const auto& item : sidecars) if(!fits(item.source) || !fits(item.dest)) {impl_->fail(INVALID);return false;}
    for (const auto& item : inline_texts) if(!fits(item.source) || !fits(item.dest)) {impl_->fail(INVALID);return false;}
    std::vector<std::uint8_t> blob;
    blob.reserve(total);
    append_string(blob, path);
    append_string(blob, header);
    const auto copies = static_cast<std::uint32_t>(sidecars.size());
    const auto inlines = static_cast<std::uint32_t>(inline_texts.size());
    const auto at = blob.size();
    blob.resize(at + 8);
    std::memcpy(blob.data() + at, &copies, 4);
    std::memcpy(blob.data() + at + 4, &inlines, 4);
    for (const auto& s : sidecars) { append_string(blob, s.source); append_string(blob, s.dest); }
    // Inline texts reuse Sidecar: source holds the body, dest the file.
    for (const auto& s : inline_texts) { append_string(blob, s.source); append_string(blob, s.dest); }
    Command c;
    c.op = BEGIN;
    c.size = blob.size();
    const void* data[] = {blob.data()};
    const std::size_t sizes[] = {blob.size()};
    const bool accepted=impl_->enqueue(c,data,sizes,1);
    if(accepted)impl_->session=true;
    return accepted;
}

bool ReplayWriter::snapshot(const void* data, std::size_t size, char kind) {
    if(!impl_->session){impl_->fail(INVALID);return false;}
    Command c;
    c.op = SNAPSHOT;
    c.size = size;
    c.kind = static_cast<unsigned char>(kind);
    const void* parts[] = {data};
    const std::size_t sizes[] = {size};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::snapshot_raw(const void* data, std::size_t size) {
    if(!impl_->session){impl_->fail(INVALID);return false;}
    Command c;
    c.op = SNAPSHOT_RAW;
    c.size = size;
    const void* parts[] = {data};
    const std::size_t sizes[] = {size};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::line(const std::string& text) {
    if(!impl_->session){impl_->fail(INVALID);return false;}
    Command c;
    c.op = LINE;
    c.size = text.size();
    const void* parts[] = {text.data()};
    const std::size_t sizes[] = {text.size()};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::frame(std::uint64_t frame, const Part* parts, unsigned count) {
    if(!impl_->session){impl_->fail(INVALID);return false;}
    if (count > kMaxParts || (count && !parts)) { impl_->fail(INVALID); return false; }
    Command c;
    c.op = FRAME;
    c.frame = frame;
    c.count = count;
    const void* data[kMaxParts] = {};
    std::size_t sizes[kMaxParts] = {};
    unsigned payload_parts = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (parts[i].reuse) {
            c.reuse_mask |= 1u << i;
            continue;
        }
        const std::size_t max_payload = impl_->queue_bytes - sizeof(Command) - 1;
        if(parts[i].size > max_payload-c.size){impl_->fail(INVALID);return false;}
        c.sizes[i] = parts[i].size;
        c.size += parts[i].size;
        data[payload_parts] = parts[i].data;
        sizes[payload_parts] = parts[i].size;
        ++payload_parts;
    }
    return impl_->enqueue(c, data, sizes, payload_parts);
}

bool ReplayWriter::remove(const std::string& path) {
    Command c;
    c.op = REMOVE;
    c.size = path.size();
    const void* parts[] = {path.data()};
    const std::size_t sizes[] = {path.size()};
    return impl_->enqueue(c, parts, sizes, 1);
}

bool ReplayWriter::end() {
    if(!impl_->session){impl_->fail(INVALID);return false;}
    Command c;
    c.op = END;
    const bool accepted=impl_->enqueue(c,nullptr,nullptr,0);
    if(accepted)impl_->session=false;
    return accepted;
}

ReplayWriter::Status ReplayWriter::status() const {
    switch (impl_->error.load(std::memory_order_relaxed)) {
        case INVALID: return Status::Invalid;
        case IO: return Status::Io;
        default:
            return impl_->pending_need.load(std::memory_order_relaxed) > impl_->free_space()
                       ? Status::Full : Status::Ok;
    }
}

const char* ReplayWriter::error() const {
    switch (status()) {
        case Status::Full: return "replay disk writer fell behind; bounded queue full";
        case Status::Invalid: return "invalid or oversized replay write";
        case Status::Io: return "replay file write failed";
        default: return nullptr;
    }
}

std::size_t ReplayWriter::free_bytes() const { return impl_->free_space(); }

const char* ReplayWriter::shutdown() {
    if (!impl_->joined) {
        impl_->stop.store(true, std::memory_order_release);
#if RECOMP_DIAGNOSTICS_REPLAY_WRITER_THREADS
        if (impl_->thread.joinable()) impl_->thread.join();
#endif
        // A producer may have published just as a worker discovered a sticky
        // I/O error. Joining settles that race before releasing leftover jobs.
        auto tail = impl_->read_pos.load(std::memory_order_relaxed);
        while (tail != impl_->write_pos.load(std::memory_order_acquire)) {
            Command c; impl_->get(tail, &c, sizeof c);
            if (c.op == TASK) {
                std::function<void()>* task = nullptr;
                impl_->get((tail + sizeof c) % impl_->queue_bytes, &task, sizeof task);
                delete task;
            }
            tail = (tail + sizeof c + static_cast<std::size_t>(c.size)) % impl_->queue_bytes;
            impl_->read_pos.store(tail, std::memory_order_release);
        }
        impl_->joined = true;
    }
    const int e = impl_->error.load(std::memory_order_relaxed);
    if (e == INVALID) return "invalid or oversized replay write";
    if (e == IO) return "replay file write failed";
    return nullptr;
}

}  // namespace recomp::diagnostics
