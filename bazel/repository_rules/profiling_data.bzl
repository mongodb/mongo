# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_6ba92dd1ca833f6c6b338aa04c201a527680ffd7_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a6429674f43a00007f74b70.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "d40ae3ad74695f82c34f3523f8ade71d0ab804ac32847be3aa9f43ad10077695"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6ba92dd1ca833f6c6b338aa04c201a527680ffd7_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6429674f43a00007f74b70.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "5d14f496adde130e58c7e35f59d531cbb48c32d05a00fe3876e6814cd96c27df"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6ba92dd1ca833f6c6b338aa04c201a527680ffd7_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6426f4a7d13c000735f25e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "c40f25947e76acf4d60eb0299d769a00efed405731ecf8cd31e31f54cd6ac774"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
