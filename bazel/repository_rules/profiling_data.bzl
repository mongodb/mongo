# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_5c293725c7a3b4fa065eafde17dc0934dac5fd11_aarch64_clang_thinlto_pgo_9.1.0-patch-6a8fae0f4669a400070d7240.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "992091866fe62d18394195871825fb1f5bfbcfb8033f4c6f7996e944c4cb1aab"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5c293725c7a3b4fa065eafde17dc0934dac5fd11_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6a8fae0f4669a400070d7240.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "16666689e774b777b601cd4918ebba7e0aead4a1b37a851dd7a7f124cf8cf4fa"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_5c293725c7a3b4fa065eafde17dc0934dac5fd11_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6a8fa8123cf883000742c244.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "11a544c9d5a020adcf020121c30cb816974869c94b4fa8eafad0e716f679be29"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
