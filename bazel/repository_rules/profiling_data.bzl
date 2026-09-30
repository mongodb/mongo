# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_e5ad5e50d3897a0204c0ff7836a9c660385f2137_aarch64_clang_thinlto_pgo_9.1.0-patch-6abc803856fba60007ebb2f1.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "7d6bcd33a95027f980c38b53810d69d19684f45d6616e07c4724b39c429885d7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_e5ad5e50d3897a0204c0ff7836a9c660385f2137_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6abc803856fba60007ebb2f1.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "f260017c8c01c885973bcd34d14f9877fe92f354572ce07511764751e321b68c"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_e5ad5e50d3897a0204c0ff7836a9c660385f2137_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6abc80c201963400074f48b7.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "c20b4ca4f8947537d1cc62ba84f8262f891f3d853d60e92da334528e3d85e3e9"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
