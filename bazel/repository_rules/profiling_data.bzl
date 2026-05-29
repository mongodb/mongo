# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_df0383b6d3010c9b06a81be7cb13fe0bfe34f736_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a1922ca910fb60007643db8.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c739a0efd53a5623853b887d0e0c2ddf97c8b109397ff6e5ba9b03a7c666e02b"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_df0383b6d3010c9b06a81be7cb13fe0bfe34f736_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a1922ca910fb60007643db8.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "90957100c684e6347239cfd847a795e7411edef92114cc36b798facd167e0a8e"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
