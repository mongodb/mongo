# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_c702b3bf3f2a8818ef5833c8fb0db652f1de6898_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a4dc0d965fa260007740ece.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "30c9a898bd40956c7ff3848a70973cd607422e4976e535d20cf6bf201da5e043"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c702b3bf3f2a8818ef5833c8fb0db652f1de6898_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4dc0d965fa260007740ece.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "c4b3544758816d8ad9bac42c84cadb00ee512ee023333253dc85bb1738b89853"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c702b3bf3f2a8818ef5833c8fb0db652f1de6898_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4dbf858ef9ae0007aca321.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "192561bbfc7b05f6088f3e1be8bcb78d2da01d26892959e1e4d8c8935bf611b3"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
