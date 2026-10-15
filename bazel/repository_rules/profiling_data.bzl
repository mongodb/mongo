# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f16d86ba1c565cb188e18cfcc8f351887ca45ed0_aarch64_clang_thinlto_pgo_9.1.0-patch-6ad047cf43e7070007baef74.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "b531ef2e437fd4ad34ac09040d18890ef1407f10ba59ccf42d531c874b4d7198"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f16d86ba1c565cb188e18cfcc8f351887ca45ed0_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad047cf43e7070007baef74.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "d9e3e1b797c8f9d39fd46977754766a5971ae3682e8ecb8e9559b55759d115ab"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f16d86ba1c565cb188e18cfcc8f351887ca45ed0_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ad0426609d74f0007740955.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "7813b7a4f3a357ba54eacaec3631302e4775fabd906fb11d6e51ab1e7e612fef"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
