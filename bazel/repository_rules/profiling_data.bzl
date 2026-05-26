# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_0bbb8b1c0435ef423df293b6af6f4a39fdb390df_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a1521a73066c20007795ab8.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "006bbf35eac4c5061b40bf578f1116a1bff574e9d4e7d12f1ff6e9506644a4a2"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_0bbb8b1c0435ef423df293b6af6f4a39fdb390df_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a1521a73066c20007795ab8.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "40d4c20d5f5c62c88e63914bd464b33ed50640f5c3fce6ce51697c7faa9b7a1e"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
