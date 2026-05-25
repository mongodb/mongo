set(WT_POSIX ON CACHE BOOL "")

# Disable cppsuite as it only runs on linux.
set(ENABLE_CPPSUITE 0)

# Header file here is required for portable futex implementation.
include_directories(AFTER SYSTEM "${CMAKE_SOURCE_DIR}/oss/apple")
