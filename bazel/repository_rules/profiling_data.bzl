# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_932b19bd21d7cde0ffd854a8eee6f9ff8936c154_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-69fc6a379dce880007cd5726.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c430b42774f1ce52233eb31838a9876c6a3bc819153c4e5ca5efd8b0423e5b9f"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_932b19bd21d7cde0ffd854a8eee6f9ff8936c154_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-69fc6a379dce880007cd5726.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "4137bbd13a2e7f340d80379acaa29d3a747fbd46d97269e7c30afd603c5d08da"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
