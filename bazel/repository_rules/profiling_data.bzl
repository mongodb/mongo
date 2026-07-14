# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_5b93964418bfd5775ab7e520eef34628e73efd46_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a55ebe0589e790007de34c0.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "221b2e1241b38cc0e590d290050323e27bdaf2ef9d267341e5eee667e41df6b6"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5b93964418bfd5775ab7e520eef34628e73efd46_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a55ebe0589e790007de34c0.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "5437c669cbf6b96add6bc2303c09d7ced3a3a63d27d44c65a41ed7f5d44e09da"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5b93964418bfd5775ab7e520eef34628e73efd46_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a55a674de15af0007055e27.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "8e90e58b0696573707a09f5531ac462ee481c63deb596f4e6b4ebe832ab73442"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
