# recomp-diagnostics-core

Private engine-neutral diagnostics transport and replay writer extracted from working engine machinery. The server binds loopback, owns bounded nonblocking line framing, and advertises registered commands. Engines supply owner-thread callbacks for pause/resume, memory, input and screenshots; no guest state is inferred here. Replies retain the existing line protocol. Each server owns its connection buffers.

The existing bounded replay worker retains its checkpoint/text/frame-hash container, recoverable backpressure and sticky errors. Hosts supply build/image/settings identity in the header and opaque checkpoint bytes. ReplayHash is a host-local diagnostic fingerprint, not a cryptographic identity. The later engine adapter must expose diagnostics and recording as a visible mod, recording enabled by default while preserving saved choices. This package does not change any consumer or claim replay determinism until integration.

CMake builds `recomp-diagnostics-core::core`; CTest exercises loopback framing/registration/reconnect and actual asynchronous replay files. Threadless WebAssembly retains the donor's explicit unavailable-writer result.
