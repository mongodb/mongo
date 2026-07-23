# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_2e02a800f6d21c1cd47aa61ed3ff14055231665d_aarch64_clang_thinlto_pgo_9.0.0-alpha0-patch-6a6187f21a210b0007113755.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "02f4c6f560fea5349a5ebe1dacead59ed5f6da79854a339cd787900a9e4ddb8b"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_2e02a800f6d21c1cd47aa61ed3ff14055231665d_aarch64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a6187f21a210b0007113755.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "dc25485860dc8136f2429c9684e24a49bfef4628c7019f54954ae556890fa8a8"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_f2ad5fe988573be2d76af85405902407d905792e_x86_64_clang_thinlto_pgo_bolt_9.0.0-alpha0-patch-6a61cd2059ff9e00078d8fb2.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "d80d253dfabb89d9da1623606f3f93c341f629a4858117cb1873bb6a76e62fe0"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
