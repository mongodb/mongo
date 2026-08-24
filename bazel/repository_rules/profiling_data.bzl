# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_108d9a60b4fae14bb6ab8fc118b1a3d95c557305_aarch64_clang_thinlto_pgo_9.0.0-patch-6a8679a46b4bad0007436f7d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "e8b122838995fde6c899ed1c7147fe6bcdb3c803b2ed8ead2acb84d53b0b4760"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_108d9a60b4fae14bb6ab8fc118b1a3d95c557305_aarch64_clang_thinlto_pgo_bolt_9.0.0-patch-6a8679a46b4bad0007436f7d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "472e241947b67daff7432b5e4ed40009dfe55b9c443c6e23b08e3608866b72af"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_108d9a60b4fae14bb6ab8fc118b1a3d95c557305_x86_64_clang_thinlto_pgo_bolt_9.0.0-patch-6a8676eaf1538e0007cb9a61.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "f87f1f5a52a937423e0e8ca01cc00044c3982b1f09ed1ce17cbbad3c72507bb1"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
