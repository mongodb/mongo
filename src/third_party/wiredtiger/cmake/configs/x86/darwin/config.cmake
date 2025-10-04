set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "darwin" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Header file here is required for portable futex implementation.
if(NOT CMAKE_CROSSCOMPILING)
    include_directories(AFTER SYSTEM "${CMAKE_SOURCE_DIR}/oss/apple")
endif()

# Disable cppsuite as it only runs on linux.
set(ENABLE_CPPSUITE 0)
