# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f28da1d4fff32b8a5504367d3e5a73e631909b11_aarch64_clang_thinlto_pgo_9.1.0-patch-6a98e68253b5b70007ee4cda.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c780ef3aa2ff4e8ad58c2d8f30f43f11eb6216b24666b5f077cbac24481a89b9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f28da1d4fff32b8a5504367d3e5a73e631909b11_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a98e68253b5b70007ee4cda.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "38d9d5124c2636750745e7fd71ccd27193e6bb1b746a43fa6ccf4c01fd3adf58"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f28da1d4fff32b8a5504367d3e5a73e631909b11_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a98e24598c9270007f1310b.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "553b2ec493574cb1c459feb834657855bd0337b8c7020e92752a514a5402a3b9"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
