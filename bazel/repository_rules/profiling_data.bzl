# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_6f0d9ff3b7393e405dfc01774bc41f9e120a5e49_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-69fdb800fa880500078452ee.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5bd9997a2a22e1e17b71d77fba0aafded10569a8ff29180e642bc9b64afd3a79"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6f0d9ff3b7393e405dfc01774bc41f9e120a5e49_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-69fdb800fa880500078452ee.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "55253c7df37e1338e15a1b67b2f1a3ce70025b7500ef3416b8224104a110110d"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
