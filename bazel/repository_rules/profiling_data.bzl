# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_019518439e10bde3ce7d8d3890eff710b949a49b_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a03314d0c5f0e000784a739.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "f0a434df784f87bd692108aa966a2b6c97711284f2a4762ee8b842a19dffc46e"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_019518439e10bde3ce7d8d3890eff710b949a49b_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a03314d0c5f0e000784a739.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "8fed9fb575991a4cec47d62ccf39278b5b807af46c25cf58dd1bb5dd10e64f47"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
