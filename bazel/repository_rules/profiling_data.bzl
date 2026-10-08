# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_d0787dfc0cad6619bc143993cc61457c8d54a845_aarch64_clang_thinlto_pgo_9.1.0-patch-6ac70cec6dfa490007219f83.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "8748bdbdc077a8764e2be8015a81a6785e2b05b4aab69e5ad889154829bb0d91"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d0787dfc0cad6619bc143993cc61457c8d54a845_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac70cec6dfa490007219f83.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "076f949075beb4707231911c2def3ebcf3f593767c22d358cbd97cea5df8fcde"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d0787dfc0cad6619bc143993cc61457c8d54a845_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac70917c16e160007cfe529.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "8eec8271672dea7174f1e6b5bae9550721133eaca8e172c1adbf37533bb0c62b"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
