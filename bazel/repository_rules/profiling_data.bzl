# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_45648501b503d45e2e1083e6937e22c7bbb74c75_aarch64_clang_thinlto_pgo_9.1.0-patch-6ac85f9d3f8a040007b832a8.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "4e8b2475afe169d2bc85651c09850aabc8718712b811f52fd41d4eb7d208d2c0"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_45648501b503d45e2e1083e6937e22c7bbb74c75_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac85f9d3f8a040007b832a8.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "e242d40054eeb8c023fb7aea455641518354bd4170b8af968d4d68a960ed69e1"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_45648501b503d45e2e1083e6937e22c7bbb74c75_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac85d1a0adf360007e06979.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "9aabbb0aff7e2d558a4e48ad1418e8676b8113b7e011742805ae0827bb04ee31"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
