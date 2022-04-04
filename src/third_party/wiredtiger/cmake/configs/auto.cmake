include(cmake/helpers.cmake)

### Auto configure options and checks that we can infer from our toolchain environment.

## Assert type sizes.
assert_type_size("size_t" 8)
assert_type_size("ssize_t" 8)
assert_type_size("time_t" 8)
assert_type_size("off_t" 0)
assert_type_size("uintptr_t" 0)
test_type_size("uintmax_t" u_intmax_size)
test_type_size("unsigned long long" u_long_long_size)
set(default_uintmax_def " ")
if(${u_intmax_size} STREQUAL "")
    if(${unsigned long long} STREQUAL "")
        set(default_uintmax_def "typedef unsigned long uintmax_t\\;")
    else()
        set(default_uintmax_def "typedef unsigned long long uintmax_t\\;")
    endif()
endif()

set(default_offt_def)
if("${WT_OS}" STREQUAL "windows")
    set(default_offt_def "typedef int64_t wt_off_t\\;")
else()
    set(default_offt_def "typedef off_t wt_off_t\\;")
endif()

config_string(
    off_t_decl
    "off_t type declaration."
    DEFAULT "${default_offt_def}"
    INTERNAL
)

config_string(
    uintprt_t_decl
    "uintptr_t type declaration."
    DEFAULT "${default_uintmax_def}"
    INTERNAL
)

config_include(
    HAVE_SYS_TYPES_H
    "Include header sys/types.h exists."
    FILE "sys/types.h"
)

config_include(
    HAVE_INTTYPES_H
    "Include header inttypes.h exists."
    FILE "inttypes.h"
)

config_include(
    HAVE_STDARG_H
    "Include header stdarg.h exists."
    FILE "stdarg.h"
)

config_include(
    HAVE_STDBOOL_H
    "Include header stdbool.h exists."
    FILE "stdbool.h"
)

config_include(
    HAVE_STDINT_H
    "Include header stdint.h exists."
    FILE "stdint.h"
)

config_include(
    HAVE_STDLIB_H
    "Include header stdlib.h exists."
    FILE "stdlib.h"
)

config_include(
    HAVE_STDIO_H
    "Include header stdio.h exists."
    FILE "stdio.h"
)

config_include(
    HAVE_STRINGS_H
    "Include header strings.h exists."
    FILE "strings.h"
)

config_include(
    HAVE_STRING_H
    "Include header string.h exists."
    FILE "string.h"
)

config_include(
    HAVE_SYS_STAT_H
    "Include header sys/stat.h exists."
    FILE "sys/stat.h"
)

config_include(
    HAVE_UNISTD_H
    "Include header unistd.h exists."
    FILE "unistd.h"
)

config_include(
    HAVE_X86INTRIN_H
    "Include header x86intrin.h exists."
    FILE "x86intrin.h"
)

config_include(
    HAVE_DLFCN_H
    "Include header dlfcn.h exists."
    FILE "dlfcn.h"
)

config_include(
    HAVE_MEMORY_H
    "Include header memory.h exists."
    FILE "memory.h"
)

config_func(
    HAVE_CLOCK_GETTIME
    "Function clock_gettime exists."
    FUNC "clock_gettime"
    FILES "time.h"
)

config_func(
    HAVE_FALLOCATE
    "Function fallocate exists."
    FUNC "fallocate"
    FILES "fcntl.h"
)

config_func(
    HAVE_FDATASYNC
    "Function fdatasync exists."
    FUNC "fdatasync"
    FILES "unistd.h"
    DEPENDS "NOT WT_DARWIN"
)

config_func(
    HAVE_FTRUNCATE
    "Function ftruncate exists."
    FUNC "ftruncate"
    FILES "unistd.h;sys/types.h"
)

config_func(
    HAVE_GETTIMEOFDAY
    "Function gettimeofday exists."
    FUNC "gettimeofday"
    FILES "sys/time.h"
)

config_func(
    HAVE_POSIX_FADVISE
    "Function posix_fadvise exists."
    FUNC "posix_fadvise"
    FILES "fcntl.h"
)

config_func(
    HAVE_POSIX_FALLOCATE
    "Function posix_fallocate exists."
    FUNC "posix_fallocate"
    FILES "fcntl.h"
)

config_func(
    HAVE_POSIX_MADVISE
    "Function posix_madvise exists."
    FUNC "posix_madvise"
    FILES "sys/mman.h"
)

config_func(
    HAVE_POSIX_MEMALIGN
    "Function posix_memalign exists."
    FUNC "posix_memalign"
    FILES "stdlib.h"
)

config_func(
    HAVE_SETRLIMIT
    "Function setrlimit exists."
    FUNC "setrlimit"
    FILES "sys/time.h;sys/resource.h"
)

config_func(
    HAVE_STRTOUQ
    "Function strtouq exists."
    FUNC "strtouq"
    FILES "stdlib.h"
)

config_func(
    HAVE_SYNC_FILE_RANGE
    "Function sync_file_range exists."
    FUNC "sync_file_range"
    FILES "fcntl.h"
)

config_func(
    HAVE_TIMER_CREATE
    "Function timer_create exists."
    FUNC "timer_create"
    FILES "signal.h;time.h"
    LIBS "rt"
)

config_lib(
    HAVE_LIBMEMKIND
    "memkind library exists."
    LIB "memkind"
    HEADER "memkind.h"
)

config_lib(
    HAVE_LIBPTHREAD
    "Pthread library exists."
    LIB "pthread"
)

config_lib(
    HAVE_LIBRT
    "rt library exists."
    LIB "rt"
)

config_lib(
    HAVE_LIBDL
    "dl library exists."
    LIB "dl"
)

config_lib(
    HAVE_LIBLZ4
    "lz4 library exists."
    LIB "lz4"
    HEADER "lz4.h"
)

config_lib(
    HAVE_LIBSNAPPY
    "snappy library exists."
    LIB "snappy"
    HEADER "snappy.h"
)

config_lib(
    HAVE_LIBZ
    "zlib library exists."
    LIB "z"
    HEADER "zlib.h"
)

config_lib(
    HAVE_LIBZSTD
    "zstd library exists."
    LIB "zstd"
    HEADER "zstd.h"
)

config_lib(
    HAVE_LIBSODIUM
    "sodium library exists."
    LIB "sodium"
    HEADER "sodium.h"
)

config_lib(
    HAVE_LIBTCMALLOC
    "tcmalloc library exists."
    LIB "tcmalloc"
    HEADER "gperftools/tcmalloc.h"
)

config_compile(
    HAVE_PTHREAD_COND_MONOTONIC
    "If pthread condition variables support monotonic clocks."
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/compile_test/pthread_cond_monotonic_test.c"
    LIBS "pthread"
    DEPENDS "HAVE_LIBPTHREAD"
)

include(TestBigEndian)
test_big_endian(is_big_endian)
if(NOT is_big_endian)
    set(is_big_endian FALSE)
endif()
config_bool(
    WORDS_BIGENDIAN
    "If the target system is big endian"
    DEFAULT ${is_big_endian}
)

set(wiredtiger_includes_decl)
if(HAVE_SYS_TYPES_H)
    list(APPEND wiredtiger_includes_decl "#include <sys/types.h>")
endif()
if(HAVE_INTTYPES_H AND (NOT "${WT_OS}" STREQUAL "windows"))
    list(APPEND wiredtiger_includes_decl "#include <inttypes.h>")
endif()
if(HAVE_STDARG_H)
    list(APPEND wiredtiger_includes_decl "#include <stdarg.h>")
endif()
if(HAVE_STDBOOL_H)
    list(APPEND wiredtiger_includes_decl "#include <stdbool.h>")
endif()
if(HAVE_STDINT_H)
    list(APPEND wiredtiger_includes_decl "#include <stdint.h>")
endif()
if(HAVE_STDIO_H)
    list(APPEND wiredtiger_includes_decl "#include <stdio.h>")
endif()
string(REGEX REPLACE ";" "\n" wiredtiger_includes_decl "${wiredtiger_includes_decl}")
