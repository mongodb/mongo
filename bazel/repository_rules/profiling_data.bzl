# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_db0eda050fb6cc453e31fc84e81e192b6d305d02_aarch64_clang_thinlto_pgo_9.0.1-patch-6a8fb08b5b0caf000759c972.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "40240beea2eff974fda942c2ceae2070c3518f1e5e58a29dc8c4ace6a9365340"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_db0eda050fb6cc453e31fc84e81e192b6d305d02_aarch64_clang_thinlto_pgo_bolt_9.0.1-patch-6a8fb08b5b0caf000759c972.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "7eaa1cd788d48c957e0e7db824f7cd1207877452476ab693893e62eac44cc422"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_db0eda050fb6cc453e31fc84e81e192b6d305d02_x86_64_clang_thinlto_pgo_bolt_9.0.1-patch-6a8fad745782dd000784598d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "abdb3593a698793ea3ecde33525879c18425231df5b3413c3d12d3b43e9f8611"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
