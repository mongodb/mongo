# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_4204e85fa76a00edc6e661a20a322d7c78aaf806_aarch64_clang_thinlto_pgo_9.1.0-patch-6af3e05dd6567500075cde36.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "e02770093b803994e939267d94f757daee00ca24c3264f55b347106e6ffb57a7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4204e85fa76a00edc6e661a20a322d7c78aaf806_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6af3e05dd6567500075cde36.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "2577ffbfcbf64987201034383ff13a43e511e23b777715495e953fe1ca504409"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4204e85fa76a00edc6e661a20a322d7c78aaf806_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6af3dcef2d05cf00075028d6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "0e70cc1221cfecbafc24845334092fb7687c1675948460a7d56b8b75def718be"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
