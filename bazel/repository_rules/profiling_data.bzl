# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_4e83499bbb1304adf3ee56f62a098f03846a2500_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a4485fbeafb7400074ab8f3.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5996f88658d4c96f3e21ffad642d63cc08da39fa59ee17483efe030f23efed96"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4e83499bbb1304adf3ee56f62a098f03846a2500_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4485fbeafb7400074ab8f3.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "eb9012274f682c89350d4b2309bbda6ce92d12441bfae2b35576fd58fae04d9c"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4e83499bbb1304adf3ee56f62a098f03846a2500_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a44870828e21a0007fdf08c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "d181e7fab4cd61c09f20ac9e88c1b5d9b5222e31c5c145ad7342d4ae800fcfad"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
