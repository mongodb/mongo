# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_b3b461f054eaf7d8fc9db45ca69cc800be61fac9_aarch64_clang_thinlto_pgo_9.1.0-patch-6abb2e3e7bc704000764a7d7.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "350d1c1c270ae7d4fd8f0cb57a8d2347ae79ed0e8efdffd8fb1185ae903d305d"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b3b461f054eaf7d8fc9db45ca69cc800be61fac9_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6abb2e3e7bc704000764a7d7.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "7fedbc4b7776dc0d8ce8cdbd509eceed270c7c24eabacd88d20e1ccf094dc88e"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b3b461f054eaf7d8fc9db45ca69cc800be61fac9_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6abb2d3b85048c0007288a42.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "f6cfa6f35f0fa0a9dd23d74b8f95dd7e9b6c7b494441b8bbe6b863fe793fcb6e"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
