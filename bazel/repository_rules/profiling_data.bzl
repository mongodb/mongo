# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_d4834185d9322a978213ecaca015d0ba00e4f16d_aarch64_clang_thinlto_pgo_9.1.0-patch-6ac073e8efbdd60007d92132.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "da02885d648018194c6534fcbb7f6549c6de217530c6eb3f2fef0d53463804b3"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d4834185d9322a978213ecaca015d0ba00e4f16d_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac073e8efbdd60007d92132.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "feba6f2ef85531530f97d94f045ec0009962f347753928bef6f94173ed4b9d94"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d4834185d9322a978213ecaca015d0ba00e4f16d_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac072ae1f95fc0007705b09.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "d11c1dc66d790e00cf29540d54bbe6b55d04ad1d370b3418ca36d229d5957a06"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
