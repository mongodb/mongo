# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_3f16c36205a2ddd53266f2cfb22c4f74864b47f7_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-69fb1538c9c1740007bd4611.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c8bd94d0f6ec4393e1eae9d1f55096f26d97b9e435bf6bb20d7bb123dee711f8"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_3f16c36205a2ddd53266f2cfb22c4f74864b47f7_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-69fb1538c9c1740007bd4611.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "e8822fd35dd6813eea647be8b1e92bfc90ce9b48c771725773b10a9c30cb79d4"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
