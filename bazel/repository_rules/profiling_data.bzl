# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_7a072ec391dcb590f919882c55a6afbb41b3c5d6_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a1a6ce98bcd590008850840.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "616dd4d2bb9ac3e187ab051af311d19684e91275625e41bbab46cb4ced659024"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_7a072ec391dcb590f919882c55a6afbb41b3c5d6_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a1a6ce98bcd590008850840.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "1204c57aeafe9713c7f88509d7fe311271ce010e68662023261b5df9908a06f7"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
