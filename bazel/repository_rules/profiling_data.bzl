# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_b62db44dd7d90da8680353a5a862f8d3b7c028fd_aarch64_clang_thinlto_pgo_9.1.0-patch-6af537b6adb9360007b43f6e.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6b9709a7ca687bc0469faea9b4dda179119a1b754cc461958b4ae114b8c9e9e7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b62db44dd7d90da8680353a5a862f8d3b7c028fd_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6af537b6adb9360007b43f6e.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "231120f27485d6e3feb5e5b362f98f2d1aba5a22f6cadc0a2242862a59e3d4b1"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b62db44dd7d90da8680353a5a862f8d3b7c028fd_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6af532f65bd87100073f9ef5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "2b9bc8467671ee26aba4a0e622611937c5b83e0bea6010f5ca53d069ea871697"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
