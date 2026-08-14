# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_d24a20a07ecee091fcd5df9862a4d34e7edaa889_aarch64_clang_thinlto_pgo_9.1.0-patch-6a7e89b1c6be710007e55eff.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "e5ef7baec60b2135dd565f92b3d763445560cc526b436f74820f4007180c811c"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d24a20a07ecee091fcd5df9862a4d34e7edaa889_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a7e89b1c6be710007e55eff.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "9a2807c61205f865997f7948fa5a8dd9608df66a0132cd7607204a43a391b816"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d24a20a07ecee091fcd5df9862a4d34e7edaa889_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a7e878206f79d0007841c8c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a454c78c7a51da6d9321aeea737aa45489229be0708e197a9da0ad79468e0313"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
