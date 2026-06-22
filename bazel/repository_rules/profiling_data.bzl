# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_47e992882b40aba8d696ff121c4dcd5ce2cf6bf9_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a36055dc56d210007107956.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "79bad742e4a9093708695202791a8e84984485b5e0db8fbe0e4f8509c9455c1d"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_47e992882b40aba8d696ff121c4dcd5ce2cf6bf9_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a36055dc56d210007107956.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "ea3edddda200aaa14f754d00786046f02e67bcde85096a16fd3ca20b82521dd3"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_47e992882b40aba8d696ff121c4dcd5ce2cf6bf9_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3605f058611900075b33ac.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "7b9af0ebdd71744e01e7ec293431d5c13bec53516831347a86faac6a83fef980"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
