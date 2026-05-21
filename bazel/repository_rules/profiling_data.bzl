# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_63e647a42504923b5c46668ff5a051994ba4863b_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0e876c5fe1860007694cdb.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "a9c81984a1df768efb9de528a196f1de1a8d3103b8a92dcc7620bf49ed090c9e"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_63e647a42504923b5c46668ff5a051994ba4863b_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0e876c5fe1860007694cdb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "193e8c681f8e2eef2b42c89571bfcf09c9bff9a0ce93ec53fa99ecbe9a7dfeba"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
