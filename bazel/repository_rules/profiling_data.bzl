# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_6c5121f9fe90cea6a52c37675d3c752f949f246c_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a5064f1f27ef50007b2f0a4.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "f7145a36aedcee697712829f586af09c1c356296a945e231fb76c0c144d083b1"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6c5121f9fe90cea6a52c37675d3c752f949f246c_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a5064f1f27ef50007b2f0a4.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "c9f1d8bf3b5c1a698c3edb58d3c79f383057b5089a4fb5cc0938208714b835ba"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_68705358f937c1a8c2a4f52b41908d138b96d868_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a50e3c3061a6a000749a5d5.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a239fe2853fd897f6173737fcf01f0c1fe2bebe3bffe60a43206e5d483775c5d"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
