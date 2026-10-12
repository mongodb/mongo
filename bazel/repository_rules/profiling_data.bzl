# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_df116d48289e3cb01d21cd13039b63988114fa90_aarch64_clang_thinlto_pgo_9.1.0-patch-6ac9af5f612abd0007b159d2.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "11bf2c2175df9c76bb4cc84978298c93b7519d43210e7b83b93a409b7642a22a"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_df116d48289e3cb01d21cd13039b63988114fa90_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac9af5f612abd0007b159d2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a44581ac0024a7f3fd8a319246f7f8763683c724f784b24becdc38a071911625"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_df116d48289e3cb01d21cd13039b63988114fa90_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac9ac1ff1663100078566c8.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "ba4a04d9ea95791d927e8fd117a986b05da9d4af950daceb16aa744a55d94c54"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
