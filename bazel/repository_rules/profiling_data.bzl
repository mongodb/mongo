# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_b840c596c0a1f61b60b0678ae28bc6b037286522_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a087d861e081e000702ee61.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6fea88e8b13fd30ecebd2da3b028608d0f249963ae6b0c38b1ce3d952fcf5326"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_b840c596c0a1f61b60b0678ae28bc6b037286522_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a087d861e081e000702ee61.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "3fdbc61861d1a0fa3a59a87bf61d56b465e4f3ac548120d07cb93dbdfac03845"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
