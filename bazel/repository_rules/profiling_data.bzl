# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_00f32958790b13d231acd445942d3ef191875ad4_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a6c13c7508f98000795a416.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "b0f194889d5f6829f448b69ea14816ba72189cc0cd4f55055ab9480ccba56b48"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_00f32958790b13d231acd445942d3ef191875ad4_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6c13c7508f98000795a416.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "f72268e1f07f69f993c6a37e0d574e79ba4e2415264b6b96a1d565ad77263742"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_fec224da209c46ba812fbabdeec84091effb4607_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a696c133706ca0007aaccbb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "66f34b08e0eec7c148bf950040c33231ee78da31f8b67ad9f11d46f87060d388"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
