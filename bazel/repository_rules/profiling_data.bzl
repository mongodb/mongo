# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_a123c44e77913944738f4cc27cad5833fca81c29_aarch64_clang_thinlto_pgo_9.1.0-patch-6ae16f0b6f0bd900071e35bd.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "bbbe442ff0bc7ef0652408c7c529d028c5a12b0abc3e4f0f4652205aa34def94"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_a123c44e77913944738f4cc27cad5833fca81c29_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae16f0b6f0bd900071e35bd.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "c4aee97d35d777576ba88032681c8061976aa00f525c9f12d94f88c791919656"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_a123c44e77913944738f4cc27cad5833fca81c29_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae16988d234ee0007929f29.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "72c0caf3c14ef45f67d4efcbae5b00efca10ec704916c3fc5f0521e70070e7a9"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
