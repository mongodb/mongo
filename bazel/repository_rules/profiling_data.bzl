# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_ddb16155a4c8216af7cd69a950192ca4a2a1b46a_aarch64_clang_thinlto_pgo_9.1.0-patch-6a89164c16c9c000075aa7d5.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "565dba2201d7ef46a8e4c587a02f4f7a11920298dc5d6a1ef744e83df90b8825"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ddb16155a4c8216af7cd69a950192ca4a2a1b46a_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a89164c16c9c000075aa7d5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "1cf0835b06771b5cbf48b1f43e3171a5614970f75333d1dec5c80ce6c54b1da3"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ddb16155a4c8216af7cd69a950192ca4a2a1b46a_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a8915366ee3910007afc781.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a98d6cf863f59f12711857e838067d303ee623d5cbe3eec6065c56917afce98f"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
