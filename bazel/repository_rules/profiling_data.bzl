# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_5d0d15677d9b2f19dcc646bb54d4086dc55051a7_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a6d6529508f9800079c048c.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "22361c867719d3a7175672c08d088b6e16c50ff74dd2dc26f9333da9ff7336b7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5d0d15677d9b2f19dcc646bb54d4086dc55051a7_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6d6529508f9800079c048c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "6c605869dd376a4eaa6c9a0c3688d4bc2c629e5fe47784ca9c64f8a8b790bd09"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5d0d15677d9b2f19dcc646bb54d4086dc55051a7_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6d63990bfbac000725fc54.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "0e144b47a10e0c03eaf35dbdfa849e4e35177469786121768f8b6d6c9a8afa90"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
