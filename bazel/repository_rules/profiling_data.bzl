# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_eebf73ce9ae7d9f5d1533c31011d804fd3a627eb_aarch64_clang_thinlto_pgo_9.1.0-patch-6aee99a1fd662500074ca647.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "fc40c0d4baa2835e1089808b439431fc2ef5c7327dccae12eca263984dfe04ca"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_eebf73ce9ae7d9f5d1533c31011d804fd3a627eb_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aee99a1fd662500074ca647.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "eceebcd0b238bb7fb5e297e8afa4121cc11ac0dbc86034caee80be84e5d68f85"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_eebf73ce9ae7d9f5d1533c31011d804fd3a627eb_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aee9816fd662500074ca20b.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a0e12b169d60356f491903421779add6f113d42540add1fa25057fff5fde838c"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
