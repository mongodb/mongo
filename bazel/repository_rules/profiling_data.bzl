# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_5da5b5e8c3dfed64b2fd62188bfd281ede455799_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a3f415f6e85840007f72b05.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "bc16b58cb0c185899b0f3ebc77f88c3d06e0e42ca03f5431858b78e8c623f698"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5da5b5e8c3dfed64b2fd62188bfd281ede455799_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3f415f6e85840007f72b05.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "9210d76a2447eb9703a14028a50467cce8dbeaa7b65ac67213cc81549b57a4a5"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5da5b5e8c3dfed64b2fd62188bfd281ede455799_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3f409ba369580007b8b838.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "84bf3e15c387743148d9efbf85c5257ca33ec90d646618592b1fa545cad8bde1"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
