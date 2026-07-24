# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_2164d3c4cf6f4cb873da646b766ef4fcd332f72b_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a62d94794e4ec000743031c.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "e56049187e7ed9e520c272ddb9af6f074153978a5c8e0a6c705665496e9926cd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2164d3c4cf6f4cb873da646b766ef4fcd332f72b_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a62d94794e4ec000743031c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "3f50bf3bef4a4e5cb6d6f8f1e5014fd955e7030df2e768eea0378357b4678c22"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2164d3c4cf6f4cb873da646b766ef4fcd332f72b_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a62d7e17dc51a0007790650.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "09f42f3bf273e3256923159128b4f6ed411f857a244d17109c9b496711b5c7f6"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
