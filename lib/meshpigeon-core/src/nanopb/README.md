nanopb 0.4.9 runtime — pb.h, pb_common.c/h, pb_decode.c/h, pb_encode.c/h,
vendored verbatim from https://github.com/nanopb/nanopb/releases/tag/0.4.9
(the files that ship in the linux-x86 release tarball's root).

Vendored rather than fetched: PlatformIO builds stay dependency-free and the
generated protobuf code (../generated/) compiles against exactly this runtime. Regenerate the protobuf sources with scripts/regen-protos.sh, which
expects this same nanopb version.
