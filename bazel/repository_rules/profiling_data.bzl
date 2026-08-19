# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_89d4a5b8849fcca877482098105b7286d30b4fa2_aarch64_clang_thinlto_pgo_9.1.0-patch-6a8521bd6c7b8c0007a53ad6.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "f43e7786a3896ba86427d3e1083a9c4f9930458b78948cf4c50ff285336aad77"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_89d4a5b8849fcca877482098105b7286d30b4fa2_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a8521bd6c7b8c0007a53ad6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "53d7713868b2287154c84ea53806a8bcdcdbda0343822ff0431f43a6b3093445"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9f0eed689cc0b8fc225c8dfe409c25d89222d12d_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a85952a573e62000758971d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "8267dedc3fedc2ab34100ebdf3eadee7a19eba2b1491977d548b4583c7a8b928"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
