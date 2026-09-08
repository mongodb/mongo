# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_db66e17361b5ad7fa40cc0f8c2adbdeda369c7a9_aarch64_clang_thinlto_pgo_9.1.0-patch-6a9f83c3713a640007f71f77.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "623d939fc42b37eaa007ba0dfc9d29c446b07601de69bee57f66c4040af10b72"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_db66e17361b5ad7fa40cc0f8c2adbdeda369c7a9_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a9f83c3713a640007f71f77.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "7f3d0b27922a220c1e2feccd78c5df791909ebb296f6a2669519b8e2a56c4d85"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_db66e17361b5ad7fa40cc0f8c2adbdeda369c7a9_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a9f80fc713a640007f7150c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "4576f85ce665af91d61d0324d332af3fe6c5093780e26c084c61ee7d5b7d9671"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
