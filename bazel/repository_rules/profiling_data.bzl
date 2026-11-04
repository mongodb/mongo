# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_446d5e8b03cba404a2fdc3d18f801492ee1bbe6a_aarch64_clang_thinlto_pgo_9.1.0-patch-6aeaa21d1e91380007e172f2.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "136228227be89cd8120b88fe26d15808398924ccd9d00779ab4d6d40684e7864"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_446d5e8b03cba404a2fdc3d18f801492ee1bbe6a_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aeaa21d1e91380007e172f2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "07f597a76418fd44c0eb9d1e6f088c2e50e38f93b9a00811af354e081a0c3dab"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_446d5e8b03cba404a2fdc3d18f801492ee1bbe6a_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aeaa174cbdd750007b47fc0.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "29e34eda8d07f9dd08ec3c08bae3affff55eb995b32e4916ffc9ae1f2277a108"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
