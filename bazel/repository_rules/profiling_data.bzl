# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_67de557cd40007f05a0ab4927a8328c4ffb18854_aarch64_clang_thinlto_pgo_9.1.0-patch-6af7cffecce5020007eac5c6.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "dc058f4425ff115710529366eeb88fb47567ccd2c7f40526d7348ac4b5b062b2"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_67de557cd40007f05a0ab4927a8328c4ffb18854_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6af7cffecce5020007eac5c6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "bbff2ba47d7a320a6d1e3617b01e4089031397ee35c04dbe82f6131d7a983d69"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_67de557cd40007f05a0ab4927a8328c4ffb18854_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6af7cd2a8fd4f20007507f5c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "0ac6877c5666b136dd7d7831fd08f3b5ac1c347bf0077c57f4ea992481894bb8"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
