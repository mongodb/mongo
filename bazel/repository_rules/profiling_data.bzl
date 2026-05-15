# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_74f2ca6e567af38e94c2c283df23ce7325af2cbf_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a05d895a8ddcd00076ee032.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "38a7db7ab29400c52d5c4b71ac12ad7f803ccf247140e801bc99cec0f56e1fed"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

DEFAULT_BOLT_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_74f2ca6e567af38e94c2c283df23ce7325af2cbf_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a05d895a8ddcd00076ee032.fdata"
DEFAULT_BOLT_DATA_CHECKSUM = "db92af0002f2ff5de5f58a12271b9fe9a9e1a526804bbc2c0aba54ba8ad33631"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
