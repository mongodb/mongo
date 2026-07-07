# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_08e25dda3d3be52d2bc6479611e1f9017f366e70_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a4c5618ec77000007690251.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "db7abe8cf77f745d09e3ea615ff81efa8a52ee5ad497d6d1855fe29d748e0129"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_08e25dda3d3be52d2bc6479611e1f9017f366e70_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4c5618ec77000007690251.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "84281e427ab8a979b2de6dbff9b29c66f86c0e482ce7e62ba06c07b299312336"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_08e25dda3d3be52d2bc6479611e1f9017f366e70_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4c5123476aef0007d34d1f.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "4636775f445b19376d8aa6acb81b10575d8314d4595d32ea6b3a4cfac1b3b77c"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
