# recomp-diagnostics-core

Current licensing and the grant for existing owner contributions are documented in [LICENSE-SCOPE.md](LICENSE-SCOPE.md).

Private engine-neutral diagnostics transport and replay writer extracted from working engine machinery. The server binds loopback, owns bounded nonblocking line framing, and advertises registered commands. Engines supply owner-thread callbacks for pause/resume, memory, input and screenshots; no guest state is inferred here. Replies retain the existing line protocol. Each server owns its connection buffers.

The existing bounded replay worker retains its checkpoint/text/frame-hash container, recoverable backpressure and sticky errors. Hosts supply build/image/settings identity in the header and opaque checkpoint bytes. ReplayHash is a host-local diagnostic fingerprint, not a cryptographic identity. The later engine adapter must expose diagnostics and recording as a visible mod, recording enabled by default while preserving saved choices. This package does not change any consumer or claim replay determinism until integration.

CMake builds `recomp-diagnostics-core::core`; CTest exercises loopback framing/registration/reconnect and actual asynchronous replay files. Threadless WebAssembly retains the donor's explicit unavailable-writer result.

The unified transport preserves qualified raw-line profiles, configurable
request/response bounds, Windows exclusive loopback ownership, up to eight
independent clients, binary-safe output and per-client reply completion. The
default remains one client with the original framing limits. Replay profiles
retain Hashed64, Genesis32 and Snes32 formats and their bounded queue defaults.

`recomp_diagnostics/session.hpp` adds one recording-session owner. Its named
GBA profile retains 600-frame/360-segment history with a 256 MiB checkpoint
budget and recoverable overflow followed by a new checkpoint. Its GB profile
retains 600-frame/360-segment history with a 64 MiB disk budget, 16 MiB queue,
and explicit stop-and-warning overflow. Configured smaller limits remain
available. Checkpoint serializers, typed inputs, hash regions and replay
codecs belong to engine adapters.

Both profiles use the existing ReplayWriter worker. Blob encoding, pruning,
logs and atomic report publication reserve bytes in the same queue; no second
worker or unbounded job list is created. Shutdown drains accepted work without
a time cutoff. Reports read only registered diagnostic files and accept
sanitized metadata, opaque checkpoints and screenshots from callers. They do
not discover original images, profile files or native saves. Recording's
visible mod and persisted on/off setting remain the consumer's UI contract.
