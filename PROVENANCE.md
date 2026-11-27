# Provenance

gbarecomp 62b6a944089b0bf8c9c699baab2c6865e9ddf1c8
vbrecomp f7dd008e09f849ceb9707aac881955b78a3e8c5d

ReplayWriter and ReplayHash derive from gbarecomp src/debug/replay_writer.*, itself ported from snesrecomp. Nonblocking bounded framing derives from vbrecomp runtime/include/debug_stream.h. Original licenses retained. New instance/lifecycle/registry adapter code uses the project license.

DiagnosticSession's blob retention, local log and store-only ZIP implementation
derive from owner-authored `gbcllr/src/diagnostics.cpp` at public revision
`5c4497090e3848c7c8c0c36ab1fc69dce7571350`, under its MIT license. The
checkpoint-text segment policy derives from `gbarecomp` at
`481b800ebeecc7f3ac1e27af104b0fe0b301b9ab`. Guest state/input codecs remain
in their engine adapters. The extracted session code is covered by the
owner contribution grant in LICENSE-SCOPE.md; donor notices remain intact.
