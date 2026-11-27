// SPDX-License-Identifier: MIT
#pragma once
#include <replay_writer.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace recomp::diagnostics {

// One producer, one bounded storage worker. Codecs and checkpoints belong to
// the caller; this owner manages epochs, segments, queues, retention and export.
// Never calls a guest callback on the disk worker except the immutable BlobCodec.
class DiagnosticSession {
public:
    using Bytes = std::vector<std::uint8_t>;
    using Status = ReplayWriter::Status;
    using Part = ReplayWriter::Part;
    using Sidecar = ReplayWriter::Sidecar;
    enum class Overflow { RecoverWithCheckpoint, StopRecording };
    struct Options {
        std::filesystem::path root;
        std::string stem;
        unsigned digits = 6, first_segment = 1;
        unsigned segment_frames = 600, history_segments = 360, report_segments = 6;
        std::size_t queue_bytes = ReplayWriter::kQueueBytes;
        std::size_t history_bytes = 256u * 1024u * 1024u;
        std::size_t segment_checkpoint_bytes = 16u * 1024u * 1024u;
        Overflow overflow = Overflow::RecoverWithCheckpoint;
        // Flat blob sessions restore only provider-owned segment names. No
        // caller-selected source files are read by reports or discovery.
        bool blob_segments = false, recording = true;
        std::size_t blob_header_bytes = 0;
        std::string log_header;
    };
    // Qualified format/lifecycle profiles; callers may reduce explicit budgets.
    static Options gba_profile(std::filesystem::path root, std::string stem);
    static Options gb_profile(std::filesystem::path root, std::string stem);
    using Checkpoint = std::function<Bytes()>;
    using BlobCodec = std::function<Bytes(const Bytes& checkpoint, const Bytes& inputs,
        std::uint64_t epoch, unsigned frames, const Bytes& final_checkpoint)>;

    explicit DiagnosticSession(Options);
    ~DiagnosticSession();
    DiagnosticSession(const DiagnosticSession&) = delete;
    DiagnosticSession& operator=(const DiagnosticSession&) = delete;

    // Streaming profiles preserve the existing engine text/hash codecs.
    // Sidecar destinations are suffixes of the new segment's path.
    bool begin_segment(const std::function<std::string(unsigned)>& header,
        const std::vector<Sidecar>& sidecars, const std::vector<Sidecar>& inline_texts);
    bool snapshot(const void*, std::size_t, char kind);
    bool line(const std::string&);
    bool frame(std::uint64_t, const Part*, unsigned);
    bool end();
    // Returns false during recoverable storage gaps. Recovery always requires
    // a new checkpoint; no old segment is silently resumed after lost frames.
    bool ready();
    bool discontinuity(std::uint64_t epoch) const;
    bool needs_segment(std::uint64_t epoch, std::size_t checkpoint_estimate) const;
    void accept_epoch(std::uint64_t);
    std::uint64_t epoch() const;
    void completed_frame();
    bool segment_open() const;
    unsigned segment() const;
    unsigned oldest_segment() const;
    std::uint64_t gaps() const;
    bool has_gap() const;
    std::string segment_path() const;

    // Blob profiles retain checkpoints and encoded input bytes on the producer.
    // Epoch changes discard an unexecuted input and close the previous segment
    // with an unchecked final digest. Encoding/hash/disk work uses the SAME
    // bounded ReplayWriter worker, never an extra queue or thread.
    void capture_begin(std::uint64_t epoch, Bytes input, const Checkpoint&, const BlobCodec&);
    void capture_end(const Checkpoint&);
    void finish_blob(const Checkpoint&);
    void log(std::string_view);
    // Caller supplies sanitized text/opaque current state. Pixel input is ARGB.
    // Archive contains only these bytes and registered diagnostic history/logs.
    std::filesystem::path report(Bytes state, const std::uint32_t* pixels,
        std::size_t count, unsigned width, unsigned height, std::string text);
    bool recording() const;
    void stop(std::string_view reason) noexcept;
    Status status() const;
    const char* error() const;
    std::string message() const;
    std::size_t free_bytes() const;
    void drain(); // shutdown/tests only; no elapsed-time cutoff
    // Optional final text record (e.g. the caller's clean-exit E record).
    const char* shutdown(const std::string& final_record = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace recomp::diagnostics
