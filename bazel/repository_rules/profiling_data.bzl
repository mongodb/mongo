# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f356c5b2a0a6f7bfd4d40523d72d086df03347a4_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a7159b1c9acca0007b20b7c.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "7e371a0bdb18d3cf071011033a29335fe5cf285211b4050b7824ec7a416dc2ac"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f356c5b2a0a6f7bfd4d40523d72d086df03347a4_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a7159b1c9acca0007b20b7c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a9ee7055192e44c4d12ffcf02dc33386f78fbff9081b1e938f6680381e018a3a"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f356c5b2a0a6f7bfd4d40523d72d086df03347a4_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a7157e29f70fc00071372fa.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "227e55bcd36e998be8c7effc7cad65e87e17b5a8b8660e024b74d5d33a6483a0"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
