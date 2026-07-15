# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_7a5c7011038f61def55745f32ca6609d199f17a6_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a570252dd4cf30007bd56f5.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "2a46501a5d3c7f58419128a381fc01b4d540e544ba22488049a3ac84fadf0f73"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_7a5c7011038f61def55745f32ca6609d199f17a6_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a570252dd4cf30007bd56f5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "3b034021a5a21d95cf57edd7f32181723ae0972ac6ee5f9ff7ed28c77df5e388"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_7a5c7011038f61def55745f32ca6609d199f17a6_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a56ffeeac175800071c833a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "f237f771b9d7ac28ced733d6fa4c6cf9f76e7f49a074f82f517597d17d03a96d"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
