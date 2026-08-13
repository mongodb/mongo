# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_1faee0501af95f1a31db9232117a0ec0af67d80c_aarch64_clang_thinlto_pgo_9.1.0-patch-6a7d3800c595fd0007313ab2.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "d6a84221f31dcacc515d9eb5ff23b60867fff1480d8e91f6b7191352eadd69d7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_1faee0501af95f1a31db9232117a0ec0af67d80c_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a7d3800c595fd0007313ab2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "869ffc0823973dca8972436385cbadc4bcd4db3bbf538e364ca245069f6fae44"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_1faee0501af95f1a31db9232117a0ec0af67d80c_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a7d379755935700075f0d26.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "7afb61f4fdf221164077ba80fb0cdfb1042a0dd93badee735d1cb32f844b800a"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
