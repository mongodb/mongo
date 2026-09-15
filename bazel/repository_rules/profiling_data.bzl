# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_3e39c05422bb0963a095e3809102931d85104de9_aarch64_clang_thinlto_pgo_9.1.0-patch-6aa8ba1d829c030007e69994.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c477371ee46332c7f4e9fdecf34ca04800cbcd9e7174f056b4918c98c9e203e1"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_3e39c05422bb0963a095e3809102931d85104de9_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa8ba1d829c030007e69994.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a73ea1aec4b4c441e0a442c9aa396207d3481178cbbfcbd38812266ff7b4c8c7"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_3e39c05422bb0963a095e3809102931d85104de9_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa8b66e5201020007fdb4ec.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "076e791f032e84b5278dc077858929fbbcd0f4d88d5fe5ebead0e80e53840eaa"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
