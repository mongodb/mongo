# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_19c90df44f0fe0321c88cca137637e9adb7ad68e_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0bdea4e8bae50007a6595a.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6cde984cb445627b9dab5b52115ddd44c562926d3d7b653feb4fade856a837ff"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_19c90df44f0fe0321c88cca137637e9adb7ad68e_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0bdea4e8bae50007a6595a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "312f118f690251e528e52fd67011d6237c7a99c2c949c5933a8c19ce9f5833f3"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
