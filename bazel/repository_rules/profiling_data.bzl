# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_a91c3cff9c7c66b0e6eb12262bb7586e05824469_aarch64_clang_thinlto_pgo_9.1.0-patch-6aa0d0dccb4aab00078353ca.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "1ec7fe7e020e5ea6b16ebbdc175ab3a62098d4e03d7642fb7d991dcc511be2fd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_a91c3cff9c7c66b0e6eb12262bb7586e05824469_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa0d0dccb4aab00078353ca.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "7d88d76e04e57da1cc6a5ac304fdb14f87a00839725c0036b83ca536feebbff0"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_a91c3cff9c7c66b0e6eb12262bb7586e05824469_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aa0cf6765df150007a17ff6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "85413eb6128434d05aee0bd57a63560d8ca015d897f938424eb10c147273bd13"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
