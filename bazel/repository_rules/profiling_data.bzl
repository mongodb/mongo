# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_0e793e59d7c483dd5c01f7be355a3f32f8b89b88_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a3b4adf56e2000007f9a3cb.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6a96a670130fed8997a67b24daf5ceca89ea3c27976bff061454d2e9b991032f"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_0e793e59d7c483dd5c01f7be355a3f32f8b89b88_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a3b4adf56e2000007f9a3cb.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "287dd12bb4b18c49b19d14c01979752750fb57b0077e70065c2edcb4237a7f2f"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_418fa6a0061b0f66c45e70fbc135421cd7ecafce_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a39f8460ed4b900075582f6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "857667fee0e0d055764dd3628fafd0c698f28826bf41d754f0e088395aa7cdbc"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
