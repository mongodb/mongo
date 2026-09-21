# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_0f1b604c554664272ee9fb3837c945278114e3ad_aarch64_clang_thinlto_pgo_9.1.0-patch-6aadff22cc6b070007cc3b70.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "20e98848ac9190f83a582a795be512dfeda529c176e5f9e0c265b9870a63ee9f"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_0f1b604c554664272ee9fb3837c945278114e3ad_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aadff22cc6b070007cc3b70.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "d7aabad8f564470f3530ea561bf48aca5e4c6b3cb8a28d39c2b7ec75d788b4d2"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_0f1b604c554664272ee9fb3837c945278114e3ad_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aadfc4b2ff4ed0007d9ed2e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "4b3a7737670c9f148deab8effcc62242d3a743ec9e695ed7efeeecd4bd195345"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
