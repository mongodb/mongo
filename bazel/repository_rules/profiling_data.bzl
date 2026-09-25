# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_8cb44297cb639912dd9e7dca375faec27cd32887_aarch64_clang_thinlto_pgo_9.1.0-patch-6ab5ea0ad1c9d9000796c91d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "14cb94c30ff619e0165b4d6a5c15df4c7dc86d26a2ecd502eb0604e888e406f7"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_8cb44297cb639912dd9e7dca375faec27cd32887_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ab5ea0ad1c9d9000796c91d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "c7c338184de8e3b6f55bbb51547425b42f0a5c99aa4ababc636f96356d584c97"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_8cb44297cb639912dd9e7dca375faec27cd32887_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ab5e7755505200007b11436.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "865816b45746612defd6f7f87fe6f1bc5b75d71395832c4d176fe2df1a3ad85e"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
