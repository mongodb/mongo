# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_9a9495de3990c0a883df8489155e54f8d7fc8b35_aarch64_clang_thinlto_pgo_9.1.0-patch-6ae2baa91ca8920007fc024a.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "24012668a7facc3ead5e672240f89574f769d98cb337c999dd914187aee19f0a"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9a9495de3990c0a883df8489155e54f8d7fc8b35_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae2baa91ca8920007fc024a.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "58d146bdc8ae244c427b58af51f8645d8ca5730e9b368319da75a02e1bb9a19c"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9a9495de3990c0a883df8489155e54f8d7fc8b35_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6ae2b79bf6a18a000785e4f2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "1860547045cfc8a62e3d7d19362477d33f1f13a45e52a98208211d79fe215ccf"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
