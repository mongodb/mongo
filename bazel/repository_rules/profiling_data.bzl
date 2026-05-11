# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_20ca2a31af90f41bd88f60f487e8a2433e70e3a1_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0092f79754ec0007aed270.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "2663ee25771d5460592f9bf8cab80952c4464c38d0397fcb0964465c057ae77a"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_20ca2a31af90f41bd88f60f487e8a2433e70e3a1_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0092f79754ec0007aed270.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "f7ea8f9a8356d559125c3e10ebe113e3fd09d68ca2d9587ce67896d9b8082506"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
