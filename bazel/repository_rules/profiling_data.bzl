# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_88607142deb2cce689bf991d9d4a56f314cba9dc_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0b8902e703d6000755384f.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "9da60ed05ffd5a6476c5c85fd0a04e44daee7212af2f222cfad6afa6559eb7a9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_88607142deb2cce689bf991d9d4a56f314cba9dc_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0b8902e703d6000755384f.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "d2e3431562ff4e3911a575b439525fc069303c87e0f0e660eb3477846371ad0f"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
