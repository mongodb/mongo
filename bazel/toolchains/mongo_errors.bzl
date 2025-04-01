"""This file contains a list of error strings that is related
   to toolchain.
"""

THREAD_SANITIZER_ERROR_MESSAGE = """
Error:
  Build failed due to either -
    - Cannot use libunwind with TSAN, please add
        --use_libunwind=False to your compile flags or
    - TSAN is only supported with dynamic link models, please add
        --linkstatic=False to your compile flags.
"""

# TODO(SERVER-85340): Fix this error message when libc++ is re-added to the
#                     toolchain.
LIBCXX_ERROR_MESSAGE = """
Error:
    libc++ is not currently supported in the mongo toolchain. Follow this ticket
    to see when support is being added SERVER-85340 We currently only support
    passing the libcxx config on macos for compatibility reasons.

    libc++ requires these configuration: --compiler_type=clang
"""

REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE = """
Error:
  libunwind=on is only supported on linux"
"""

SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE = """
Error:
  fuzzer, address, and memory sanitizers require these configurations:
      --allocator=system
"""
