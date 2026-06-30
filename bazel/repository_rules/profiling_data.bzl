# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_38b6a99820f8fa0abe2e6862843a2cedba615cc3_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a4334eb2df00200079b2ee6.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "17b0d8e7fcc05b483d318cef808d8e3c6fd6a4f3493aea6e0439e3faf3194f70"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_38b6a99820f8fa0abe2e6862843a2cedba615cc3_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a4334eb2df00200079b2ee6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "d83771ce361426e684c41e988e825c38f4a4c0abb69d69554ba7b0cdbc9c9fce"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_38b6a99820f8fa0abe2e6862843a2cedba615cc3_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a433220e71bd90007b2d2c2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "e32ad3214b46e53281f0d6e949cac27ed211cc06c5d5fbdb5687ca9f9c7caa0a"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
