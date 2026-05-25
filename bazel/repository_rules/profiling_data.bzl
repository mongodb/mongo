# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_2361b3ed4cd11abc3811b085026c07ec0c6e6b15_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a13d20c8ee9720007362c0d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "007e7c7ea30a7ce1ed8e2768773bb808352ef3be3df79579e87423d72b7a40fd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2361b3ed4cd11abc3811b085026c07ec0c6e6b15_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a13d20c8ee9720007362c0d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "e7a19eac7caa3013c90c0609d5b84f87c89f3b5dd322b56baddec65cda53e8b9"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
