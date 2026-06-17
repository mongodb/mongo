# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_630acb15ae985b0524c9288497837990140a7aa1_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a3211c31aa3e8000793da31.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "490ae63548b8e0d9cf07038764937cac03dbd15517dff181e1bde8e62f07f633"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_630acb15ae985b0524c9288497837990140a7aa1_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3211c31aa3e8000793da31.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "3456312068b1c1c37aa5e69e7b8f9c1edd34d54259ea7989455a9db2a8ba99d6"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_630acb15ae985b0524c9288497837990140a7aa1_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a320fc487cc870008f0d4fa.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "2ba5df3613176886a6b0515562d37f95b9eed73bed8a11a53ec01fd266b9bfea"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
