# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_4a19eb6224a490d6a67c1995e549ec2527948cae_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a0d2ebeb907d2000753324d.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "6ffeed2240fdaf25cb0d43fda5934721adc1d96f18f87d240289ce801acf3389"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_4a19eb6224a490d6a67c1995e549ec2527948cae_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a0d2ebeb907d2000753324d.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "c2f0b7937f34f3704118f86a472760bd8062e61e89d1616c48a8cf40364a1685"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
