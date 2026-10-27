# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_ea4adc6d9c43c437482d242285841f0e0f5834bc_aarch64_clang_thinlto_pgo_9.1.0-patch-6ae018b0dffbb50007cd057e.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "bfa3851eaf205d33450658a7fafc55133c92ed4d28951995babcf08380aa6fbd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ea4adc6d9c43c437482d242285841f0e0f5834bc_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae018b0dffbb50007cd057e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "7252dc37889905e8b8aa2cf143576bc21de2527d0de258423f4c228b6f14bfb0"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ea4adc6d9c43c437482d242285841f0e0f5834bc_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae018fef4a1bd00072eb070.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "603bfe4f6a86e359802a955f65a5171cef9a305a8ad49b0a5bce9398d1268b76"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
