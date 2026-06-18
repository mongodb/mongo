# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_10481703f099a92c0a3fcc368be8e0a90695bdb4_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a33609d59380e0007248c2a.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "d37562ce0ea0dec64fbd919500d7ee8756cae187f55ea80f5cbd3be0cb7de4bd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_10481703f099a92c0a3fcc368be8e0a90695bdb4_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a33609d59380e0007248c2a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "25a5b8d372d5587528a2ac9803d0a484febc4ccd0f33a3f6c744fdc12396f34d"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_10481703f099a92c0a3fcc368be8e0a90695bdb4_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a335f7ec13ae3000714c1f5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a3ce4c39367cb54d32087816d259aba211ae23d2d94364c750ac451ab02c7e28"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
