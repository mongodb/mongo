# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_2593fca571ed8bffd3be94ecbb7f7251ca857f23_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a2a429ffdf3c400075b1c88.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "24fba46e7d507cbc6004cd002f7587ffdc36416c249e91730748aa62f12ae79f"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2593fca571ed8bffd3be94ecbb7f7251ca857f23_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a2a429ffdf3c400075b1c88.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "73b9ae1666673fcc911b50169d8bef151e9d1056433e26183905b126aed5fc73"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
