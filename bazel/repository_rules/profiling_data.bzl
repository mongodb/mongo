# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_d505698c3124bf293a81798f069b00b0de6bf56e_aarch64_clang_thinlto_pgo_9.1.0-patch-6a87c4e3819e1000079a58ef.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "050780dce709b4aa1b5c1be114f424d2590a1e429129957fe384e7e74d67e984"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d505698c3124bf293a81798f069b00b0de6bf56e_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a87c4e3819e1000079a58ef.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "e70b37fd7d6766f381e329bd9e42f3ecdabce29e4e4a4c0766ea0c55c8f1af30"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_d505698c3124bf293a81798f069b00b0de6bf56e_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a87c43aeaa8ae000706cc8b.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "5bef5af67bf099feb645b5f04524c25a08034237c263181e89b41f6aaabb96ee"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
