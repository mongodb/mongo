# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f6c5c03d036952bb603a8be45639ea93122d89f3_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a584e5ab564ce0007738dc7.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "13910e3aa5d5c4a7971cbd3aaa4736a815a33eb8be05357dd16ff8b5e481f755"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f6c5c03d036952bb603a8be45639ea93122d89f3_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a584e5ab564ce0007738dc7.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "f31e7d9da4dda1636ff9b5cca8a0fff89ec1cbccd44ad5f356fa9028e19d0ab8"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f6c5c03d036952bb603a8be45639ea93122d89f3_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a584d8faa73bb00071409ce.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "02a29c9b34485aa231c57650873280856f5af0d16daebb66c06d7d4b4797c3dc"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
