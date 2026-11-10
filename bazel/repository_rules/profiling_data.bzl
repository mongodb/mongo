# This file gets automatically updated by profile_data_pr.py. Do not change the path to this file or variables in this file
# without updating that script.
DEFAULT_CLANG_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_9e9578ca8e9fe10850b392e1828338f6d4edf2ef_aarch64_clang_thinlto_pgo_9.1.0-patch-6af28cde0366280007c0436f.profdata"
DEFAULT_CLANG_PGO_DATA_CHECKSUM = "c0b23feb8db40494de616027dacbcb608eff5ca7950a5e8c89bac205d4136dd9"

DEFAULT_GCC_PGO_DATA_URL = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/pgo/mongod_efcbfdbb937f52078925254ed32fbca7901b4ae6_aarch64_gcc_lto_pgo_8.3.0-alpha0-1055-gefcbfdb-patch-68bfb348576a720007510f50.tgz"
DEFAULT_GCC_PGO_DATA_CHECKSUM = "29b9d919abdccb4a2eeb38670e0489312792700559eb7282e0b02fe2f5ec7744"

# BOLT profiles are tied to the binary layout of each architecture and can never be shared,
# so there is one entry per architecture. PGO profiles above are IR-level and source-keyed,
# so both architectures intentionally share the arm64-trained PGO data.
DEFAULT_BOLT_DATA_URL_ARM64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9e9578ca8e9fe10850b392e1828338f6d4edf2ef_aarch64_clang_thinlto_pgo_bolt_9.1.0-patch-6af28cde0366280007c0436f.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_ARM64 = "81655592904411b99bdc0305bf6f208ace266f505b899028e362b24d00eb6d37"

DEFAULT_BOLT_DATA_URL_X86_64 = "https://mdb-build-public.s3.us-east-1.amazonaws.com/profiling_data/bolt/mongod_9e9578ca8e9fe10850b392e1828338f6d4edf2ef_x86_64_clang_thinlto_pgo_bolt_9.1.0-patch-6af28b21eb666600079fbdbd.fdata"
DEFAULT_BOLT_DATA_CHECKSUM_X86_64 = "38001df6df61a30419470dfbfff974e0187e7ba59ee296f87f19a0cc0f51eeee"

# CSPGO is a pre-merged profdata combining stage-1 PGO data with stage-2 context-sensitive
# data. Populate these once a profile has been generated and uploaded. This is currently
# unused as it does not show significant performance improvements.
DEFAULT_CLANG_CSPGO_DATA_URL = ""
DEFAULT_CLANG_CSPGO_DATA_CHECKSUM = ""
