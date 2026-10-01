# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_5e1790312853461f53451c0d3a4aecab7d8f112a_aarch64_clang_thinlto_pgo_9.1.0-patch-6abdd23ccb04b20007e22cc3.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "b9cbaf3e6c9682bbdf7616a4e780a36b6e0e6d2e56b50bf3aaee7f409cc2b512"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5e1790312853461f53451c0d3a4aecab7d8f112a_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6abdd23ccb04b20007e22cc3.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "bfcacc65268df2c01e4ac195bfddd60dbd943f5f477694427ec66304f4288792"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5e1790312853461f53451c0d3a4aecab7d8f112a_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6abdcdc3a8d3f30007ce6dc9.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a3b82a8300e4a4456734b28d096916a4060a839c30b303fbe5876a9da6ec5d2e"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
