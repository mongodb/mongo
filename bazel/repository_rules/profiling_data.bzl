# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f69b485561f52e57fa72428241c3d3352671b2bd_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0484ed374093000795b9b5.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "0010e91b0d4723280805975fe12ef20fc3a7eb92eb12deec03960b96dcf4d9c0"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f69b485561f52e57fa72428241c3d3352671b2bd_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0484ed374093000795b9b5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "358e9fcf8c62c93c98a6b664a4b84cfb00c6a030e95abd92d5d74cfdd2e60616"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
