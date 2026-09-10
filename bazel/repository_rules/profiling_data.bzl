# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_11e9be299070987fc9fefdc8f8789f0ac6003271_aarch64_clang_thinlto_pgo_9.1.0-patch-6aa22108a5813a000766091a.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "4768b02f0e53ac6e3227b635e1d8880418650639d8ecdbfa68f5486b8af53213"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_11e9be299070987fc9fefdc8f8789f0ac6003271_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa22108a5813a000766091a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "ef22cf3bcb14c561ffc48b888ece22d37155d9199b10045f90900adfc6520b80"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_11e9be299070987fc9fefdc8f8789f0ac6003271_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa21f27b47e880007a954f9.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "f741cae09acb7c3d86aa1f5d954fe080324f4a4dee7371c110327b0654d459df"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
