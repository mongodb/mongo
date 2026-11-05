# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_fabb36b8d55ef3e20e7d88aa0fbda11b440f0805_aarch64_clang_thinlto_pgo_9.1.0-patch-6aebf56d22d19a000752d711.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "f535853976181be07e541c21daa2e3ce4f107c5c1bb4d5fb0bd3f0728c267527"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_fabb36b8d55ef3e20e7d88aa0fbda11b440f0805_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6aebf56d22d19a000752d711.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "2f352ea8cf0883bf080df0ceb2715ee78a3ca009631a2f170db029d44c375adc"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_fabb36b8d55ef3e20e7d88aa0fbda11b440f0805_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6aec1c8b83ce9e0007d2bc63.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "c08c66adde590ad9d71283a52621c66856ddeb1e97ee702c4a68fd953d16b75b"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
