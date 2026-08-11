# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_60d7e4de2955991ffb450f614925f043615442d1_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a7a95316ef7660007762231.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "5ff9e39d117683ecfed2fe1d4f523c5708b427641f50904f1afed466559c6a27"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_60d7e4de2955991ffb450f614925f043615442d1_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a7a95316ef7660007762231.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "f05f6b7b4cbd7f31018d993c9db68c82694de2db8c4b2c05091e107aaf45f89c"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_484ead498553995aff366564dfb69dd66df3892d_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a754dfb3dd9d400073d227c.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "7e9966f38c78b1f11762e10fed0c37968e61bacac181401130d34c36cbe19f73"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
