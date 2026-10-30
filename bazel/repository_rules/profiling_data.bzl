# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_24e5260d103e32c9e956db489736d349c5a0e333_aarch64_clang_thinlto_pgo_9.1.0-patch-6ae40ea2393be9000727d510.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "da4d5daa1fadcfd0b60eaabb6be6b58b3f1d05ebf258cfa0b4b64bd02f1d9c41"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_24e5260d103e32c9e956db489736d349c5a0e333_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae40ea2393be9000727d510.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "e6d50346cba7649f4c046714cb5bbcc95b6bbe5ba88c6bddc8d1daedb630f919"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_24e5260d103e32c9e956db489736d349c5a0e333_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae40e4d401fce0007ad33d1.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "b75bdea5218bbcf5245f7fadd36b2b4ea1a6649a9949367270f54fefd6fe3785"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
