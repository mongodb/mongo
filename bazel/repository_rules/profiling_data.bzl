# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_39b829b80af507ac5b1e410327d5cc0a84c347e1_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a73fa86ea42ac0007906edb.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "b9436ec600386a07346235e3b6a28d8f55305f57a71c621d163d0998e1649fdd"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_39b829b80af507ac5b1e410327d5cc0a84c347e1_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a73fa86ea42ac0007906edb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "aa08e3b8350721487663ee38e2fc74dc1ce275effec19b7618c97cb5e97c5b69"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9191071a830f9c93d82c8cbdcf980d2b93ef1893_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6186b3d9bb9f000706a1fb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "d80d13ccca88c7093435e269343cd80b0109594710de08ca96a13b4cb54cfebf"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
