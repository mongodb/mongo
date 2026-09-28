# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_05cd754f299dbe2ce2871ae08b3df7f25329d92e_aarch64_clang_thinlto_pgo_9.1.0-patch-6ab7388097eb1c000735c349.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "7a0971e16ca7253550862a3c0c727f862a6a1618d011a0aa579eb153d4f7885e"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_05cd754f299dbe2ce2871ae08b3df7f25329d92e_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ab7388097eb1c000735c349.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "9f5bdc3d7facec63b299bdfa5ff4659ab90d8e0087de62a6e0cc049590233483"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_05cd754f299dbe2ce2871ae08b3df7f25329d92e_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ab737701678ea0007b3e1ba.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "37b8e338bd17e8a4c735cb9107e4460437bd87ed8080eb90305f1c3ba968352d"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
