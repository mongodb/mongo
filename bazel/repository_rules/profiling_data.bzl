# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_beb26fe57cac6efc1b34ea759534c26026e83478_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a3c9db48dea400007851caa.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "42fdb9fb2141fe762d00dc0f1eb719a3c99cd1feea04a82bb73e84522f4b1e84"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_beb26fe57cac6efc1b34ea759534c26026e83478_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3c9db48dea400007851caa.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "10643cb0a8a93369b93d0b1fe2b2feda047b3dfb4dcfa385d750cbc559e823ff"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_beb26fe57cac6efc1b34ea759534c26026e83478_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3c9acc4706610007439694.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a2e7b7b1b812b5a865db26bc20e08ee7ecc62ba5900307fc1166ded16b65907c"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
