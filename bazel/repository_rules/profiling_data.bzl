# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_b10821b6c90a1c940e2adb3b52459ffeae4f2733_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a768631f62c6c000735cbe2.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5d3d40292d9db72e34e8de4e00835c7a0503b863c00d4bf5bc6962824258381d"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b10821b6c90a1c940e2adb3b52459ffeae4f2733_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a768631f62c6c000735cbe2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "81ba4876b413e38801e710b8d1ba073a605083482ee929128a78b62f4c818860"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b10821b6c90a1c940e2adb3b52459ffeae4f2733_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a76839a3869cd0007c3b69c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "510f6d2e9b56786f807c7484240365d0cd942d55b273cf4a078db19ce2c51e4a"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
