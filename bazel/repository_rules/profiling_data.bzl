# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_63517f4eb0d067a1d6af2409de65a60a0531dd6d_aarch64_clang_thinlto_pgo_9.1.0-patch-6ac46996d43f8800072291c3.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "1cdde633c034b84dbf4ea3d02b800d00dbad6eed1112330f34361923ffd02899"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_63517f4eb0d067a1d6af2409de65a60a0531dd6d_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac46996d43f8800072291c3.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "0bfa01f6e45fed349f154521e00b656bf497c3e8c4107db6a3d42e0d51b7f57a"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2681a71bf35311024ff7a43926dc2dc432aa971d_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ac5bafc1f083b00075974d7.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "31e6e497630bc4c5ae3de492843d66760c373af5993c4496afc219b466d9ef28"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
