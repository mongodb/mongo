# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_ee915e4d7843f22c0c127e8daf8b853773682d3c_aarch64_clang_thinlto_pgo_9.0.1-patch-6a98e43e18fc4900079aef3f.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "05e6100ce41f6deffeba7641e21eb6f02fe96d83191921af5f13ec728ccf745c"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ee915e4d7843f22c0c127e8daf8b853773682d3c_aarch64_clang_thinlto_pgo_bolt_9.0.1-patch-6a98e43e18fc4900079aef3f.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "a10176215c17338e3782978d8e635cfadc99319d6f90577e23183d783dac944b"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_ee915e4d7843f22c0c127e8daf8b853773682d3c_x86_64_clang_thinlto_pgo_bolt_9.0.1-patch-6a98e1bf18fc4900079ae3a1.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "cda8d001f0c535cddb965e66817970e2cace57ecb8be7dfd9d386d4deb27deec"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
