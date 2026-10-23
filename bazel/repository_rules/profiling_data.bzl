# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_203cea53a8605e919ac762f6305265d6f8d747cd_aarch64_clang_thinlto_pgo_9.1.0-patch-6adad4c4603575000738623d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5c1ae7ceac1792bacd9ad18b18727fcfc4dde5eb8ec9473a909e38bb89148871"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_203cea53a8605e919ac762f6305265d6f8d747cd_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6adad4c4603575000738623d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "03440186fc0c0ebbe039be3cb98eba1c23898d5718149c4120150a6153957ed7"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_203cea53a8605e919ac762f6305265d6f8d747cd_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6adacfe8ea54bb0007bf5963.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "a0eb5d5ef6f5f855d6e9d1913377f554b93201a335d0077245602045f40653b8"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
