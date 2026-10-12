# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_6b5787c43d011981395c54179e4017f3c230a526_aarch64_clang_thinlto_pgo_9.0.5-patch-6ac710e4c5785400072b0cb6.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "482b837a691da6becb2187f58eec9b85f7b7dfcc2c37e673de01988fce07d086"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6b5787c43d011981395c54179e4017f3c230a526_aarch64_clang_thinlto_pgo_bolt_9.0.5-patch-6ac710e4c5785400072b0cb6.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "092d828f13baa8a56ccf94d7cc2f7fa3f91bae0b4c04adb502ec22b98d109fc4"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_6b5787c43d011981395c54179e4017f3c230a526_x86_64_clang_thinlto_pgo_bolt_9.0.5-patch-6ac70d39d3858b0007963dad.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "401ada79e315a1b0962ddd364518ae399632098dc1c9ea8ac6209152f6cb7ba6"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
