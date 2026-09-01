# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_f9826f7e91290930b8c3afe31124ae282ca1c89c_aarch64_clang_thinlto_pgo_9.1.0-patch-6a90fef25f147800076c6dae.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "68a3082d4184c73d133ac119a773bb343727de4e81402c52daff4af486c144e6"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f9826f7e91290930b8c3afe31124ae282ca1c89c_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a90fef25f147800076c6dae.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "4223da66e81854fc7389669c8963e26e463864d38ad7fb788f9479a1e7923cf3"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f9826f7e91290930b8c3afe31124ae282ca1c89c_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a90fcb73cb6d30007734316.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "d5672bc463b5af25fc6182857c781f25f49bd386ed9adf0bf0132a8d4cd630b2"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
