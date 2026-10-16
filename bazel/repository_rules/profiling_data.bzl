# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_defd142d89a0f8167619fdf2b87a36729a8e994d_aarch64_clang_thinlto_pgo_9.1.0-patch-6ad196e346c8f40007494ecb.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "a4245ef0c07b15773343d6a6e501a4ac310c3e448f2bba4d708d81a342270cb9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_defd142d89a0f8167619fdf2b87a36729a8e994d_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad196e346c8f40007494ecb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "c05cf730b6b4ab6cf4608da25ca207f76c6193b5c00b68993521baa960d0b053"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_defd142d89a0f8167619fdf2b87a36729a8e994d_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad197de13a8180007ea478e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "274995d7d6ef00f6ce0a280510534fde55829ddb80f79a11569529b90a7a87ed"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
