# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_12a249ef45fd7807e6a373e66f2ea12cc11b6668_aarch64_clang_thinlto_pgo_9.0.5-patch-6ad0452ed701a00007ccba4b.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "36c5b618547bf0f3b97c4440e6d135bcb194d0b437bb38c799ce7df0a0a178f9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_12a249ef45fd7807e6a373e66f2ea12cc11b6668_aarch64_clang_thinlto_pgo_bolt_9.0.5-patch-6ad0452ed701a00007ccba4b.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a73ce0ffd2c275108e81680d47efd2b9ebe8d67e4874610dd13b0e58c2220dc9"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_12a249ef45fd7807e6a373e66f2ea12cc11b6668_x86_64_clang_thinlto_pgo_bolt_9.0.5-patch-6ad0420c1d798100078f006a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "af3d78b66d061424922e9e7615380b9ed1fe8fd94417dc62f9ef0f795f51f4bf"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
