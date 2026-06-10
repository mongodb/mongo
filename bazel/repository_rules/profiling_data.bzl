# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_368c776be8dc6392c725a4ae942c404747de1693_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a28ee2ee86b0c000726dd20.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c3c8533a960db125219996fda4116332414488f14ef8073bc51cb601dc7b28ed"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_368c776be8dc6392c725a4ae942c404747de1693_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a28ee2ee86b0c000726dd20.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "e412107ba668d0be94056f117eb115233cdd066a3c9f1d41ed99f7ab04bf3d53"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
