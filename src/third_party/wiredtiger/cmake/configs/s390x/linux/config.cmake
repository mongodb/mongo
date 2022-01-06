set(WT_ARCH "s390x" CACHE STRING "")
set(WT_OS "linux" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. Access to O_DIRECT on Linux. Append this macro to our compiler flags for Linux-based
# builds.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE" CACHE STRING "" FORCE)

# Linux requires buffers aligned to 4KB boundaries for O_DIRECT to work.
set(WT_BUFFER_ALIGNMENT_DEFAULT "4096" CACHE STRING "")

# Allow assembler to detect '.sx' file extensions.
list(APPEND CMAKE_ASM_SOURCE_FILE_EXTENSION "sx")
