# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_c12415625c977326478b2461f0c7976416f62cb1_aarch64_clang_thinlto_pgo_9.1.0-patch-6ad2eaceeb244e0007e97110.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "729cad9281225a189ba7114fb68e0464c9c72cc87d5db9c2ecc85808f661d960"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c12415625c977326478b2461f0c7976416f62cb1_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad2eaceeb244e0007e97110.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "e0e4a6d3af7beb08dec144bcd148c9554819fe05fdb769173392b9b04ca9ff91"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_c12415625c977326478b2461f0c7976416f62cb1_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad2e82de103fc0007e7f452.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "18ba84fd0f6d96aaaff3d593ed9d8cd79aa8dd30d9fa7c30d9c06d504c750b0d"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
