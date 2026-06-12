# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_4db9523a32d465c46be99e422c1fb6ca4ccb8019_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a2b939e69a09600076d69dc.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "282fc21b2e26439004817c1e3e3a1de0a98a9acb57cc00fc139d48fcaab9f30c"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4db9523a32d465c46be99e422c1fb6ca4ccb8019_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a2b939e69a09600076d69dc.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "bb1a6e1628747281f6a75dbb54cbda9f827801eba3fdf87afe1161f372e96be7"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
