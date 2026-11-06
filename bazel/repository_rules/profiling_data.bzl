# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_220960b4f1d4311ae4816cccd922cb3ea429a058_aarch64_clang_thinlto_pgo_9.1.0-patch-6aed44ea52135f0008f6a0da.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6e547bc68bd7645a8847135654b9b04a595a10849256adbed483e9b0bca5f4cb"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_220960b4f1d4311ae4816cccd922cb3ea429a058_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aed44ea52135f0008f6a0da.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a41b2139d17a175adb61dccb1c85f766af21da718a8cd834b96e44695308cb25"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_220960b4f1d4311ae4816cccd922cb3ea429a058_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aed42aa025ce0000737c903.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "1bc61736c8da8917db4a390b0cf1782747336c355f71955962a8cf475955e692"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
