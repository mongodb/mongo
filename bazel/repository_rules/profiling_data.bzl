# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_9b7e7a8fd3c6c053e1e497c21535590252bbf13a_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0fd9e72a05ef0007eb8971.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "a3698b33277ac142d50b28467f5aaff34dc6e9ae7318c2ff52f55ddef006ad50"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9b7e7a8fd3c6c053e1e497c21535590252bbf13a_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0fd9e72a05ef0007eb8971.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "91535f9262ab196364d155310a7aff6911402a64d1dfafe685acc0e976787cd6"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
