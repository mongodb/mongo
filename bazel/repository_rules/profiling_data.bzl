# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_065c02b56f788f57bdc6f724a9292fa2dba210d2_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a68222429359500079c0c3d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "cfdf41ef5e13138abf39bee22c06756e94c4e08c2983a3d0a9d67c2f5dc56cd9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_065c02b56f788f57bdc6f724a9292fa2dba210d2_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a68222429359500079c0c3d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "f5f8341de0285639bb4b8bc4f3502df050317331da8744b43c0d295ff021e8ba"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_8fe1b46b22a3ffaffb78431ec242920092193cf6_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a68ab80df1e7f00072f3f90.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "15e68d81c438af47c6681e44653c3853a0627ec338d21bdadf50483c4cef61ab"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
