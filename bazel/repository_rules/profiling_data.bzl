# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f01538c67f8569cd9cdf574411b4f078c4f6027f_aarch64_clang_thinlto_pgo_9.1.0-patch-6a9644080af00700072ad85c.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5ef7147e4e2cd0dc8670f2fa22ea84818be041f17e0c614bd133d1a8be768431"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f01538c67f8569cd9cdf574411b4f078c4f6027f_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a9644080af00700072ad85c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "b1b4c5b99c454a6f22eae33a4d9578260604a447b37616b580339aa9d8c74ec9"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f01538c67f8569cd9cdf574411b4f078c4f6027f_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a964105e8c487000743dcbf.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "09f17c12dee9f1d5f0efe5640ba07bcd3c294bedf4225e8292971cd06523f535"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
