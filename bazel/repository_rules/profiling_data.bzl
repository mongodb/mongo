# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_51a4398d0cfbb3a202bb869d2aabb48bd91cc4d9_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a3df25858cd8800071f2dfb.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "a9356c9d8833fbb6c5a1e00391da67a15b0dcc756e8b80b2519b8f63623c544c"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_51a4398d0cfbb3a202bb869d2aabb48bd91cc4d9_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3df25858cd8800071f2dfb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "97a8a8c0c2456755113641a3f4c7981637f5c081bc93edabf937e5029c77f4f0"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_51a4398d0cfbb3a202bb869d2aabb48bd91cc4d9_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3deead4a808e000707266e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "eec1dbb53e7ff3c1b82a66ca0671b59dc03dd0c805a41f7a1bbe35e14d6914c7"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
