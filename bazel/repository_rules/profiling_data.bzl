# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_c2efdac0ad822d06a444d4784d383651ed634dcc_aarch64_clang_thinlto_pgo_9.1.0-patch-6a9796ceb6afb40007043c71.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "7ec5f8205f38ed891ee9238409719d005279414c0560efd3258f480e5fe90689"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c2efdac0ad822d06a444d4784d383651ed634dcc_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a9796ceb6afb40007043c71.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "d9b86adc72dc8e8390ca043ed0cc1b4828f5b47f6d5ce755f730309380f1555a"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c2efdac0ad822d06a444d4784d383651ed634dcc_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a97969756cbbb0007773bf8.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "e16476dc1192de778651ed2c66d7f23d3e11e0ddf88d1045920fb0447cd6144f"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
