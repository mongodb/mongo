# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_3bd0e4c25a5bf81821848f436bc92869a9821b34_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-69f9c52c3c2452000885963a.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "0ea7fb7d6f860a4b94cc3fd07368b5c666c5683419ee0e3ecb938fa9adff05df"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_3bd0e4c25a5bf81821848f436bc92869a9821b34_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-69f9c52c3c2452000885963a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "a4a5f811273908fcd3bbabe1a7c5acc57dcad7d129e5ee5293b9d2e038638e62"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
