# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_bf438acd69469fef820db80b24edafa509d91ab0_aarch64_clang_thinlto_pgo_9.1.0-patch-6aab5bb09495ce0007a44071.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "bb915a7008dcb058d638c6ed1819eeac82e6f6d1aec56f88608df07af45756e1"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_bf438acd69469fef820db80b24edafa509d91ab0_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aab5bb09495ce0007a44071.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "5e8782ab21acd8f25de6deb324e8ce87ed9958d3b14b653fc7a37775839ae41f"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_bf438acd69469fef820db80b24edafa509d91ab0_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aab5a4d89b0d60007177e4f.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "e88dd10e3d9af22ae335e5c19a9ba4b89e5cd0d5825466094525b1973805c8ba"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
