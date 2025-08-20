include(cmake/helpers.cmake)

### Auto configure options and checks that we can infer from our toolchain environment.

config_include(
    HAVE_X86INTRIN_H
    "Include header x86intrin.h exists."
    FILE "x86intrin.h"
)

config_include(
    HAVE_ARM_NEON_INTRIN_H
    "Include header arm_neon.h exists."
    FILE "arm_neon.h"
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
    HAVE_CLOCK_GETTIME
    "Function clock_gettime exists."
    FUNC "clock_gettime"
    FILES "time.h"
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
    HAVE_LIBCXX
    "stdc++ library exists."
    LIB "stdc++"
)

config_lib(
    HAVE_LIBACCEL_CONFIG
    "accel-config library exists."
    LIB "accel-config"
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
    HAVE_LIBQPL
    "qpl library exists."
    LIB "qpl"
    HEADER "qpl/qpl.h"
)

config_lib(
    HAVE_LIBSODIUM
    "sodium library exists."
    LIB "sodium"
    HEADER "sodium.h"
)

config_compile(
    HAVE_PTHREAD_COND_MONOTONIC
    "If pthread condition variables support monotonic clocks."
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/compile_test/pthread_cond_monotonic_test.c"
    LIBS "pthread"
    DEPENDS "HAVE_LIBPTHREAD"
)

set(WORDS_BIGENDIAN FALSE)
if(${CMAKE_C_BYTE_ORDER} STREQUAL "BIG_ENDIAN")
    set(WORDS_BIGENDIAN TRUE)
endif()
