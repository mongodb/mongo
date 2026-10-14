# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_dea37daf08a59d1f88077c8815b03b5b780a95f3_aarch64_clang_thinlto_pgo_9.1.0-patch-6acef7571a6e0d0007696e82.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "437ca988a525661a3fdce337339aa466a3f63a94e058554790e4a9da2ea09312"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_dea37daf08a59d1f88077c8815b03b5b780a95f3_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6acef7571a6e0d0007696e82.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "73bed06ff0a31408ad953d1df3aa8c13c90357270c1b187c51f1847e7853bfac"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_dea37daf08a59d1f88077c8815b03b5b780a95f3_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6acef724c7bf940007f64f44.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "890e656ed9eddadc572c4201b98a410609021d0bc4e7c20e3f8fa1526b3050ab"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
