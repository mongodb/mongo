set(WT_ARCH "x86" CACHE STRING "")
set(WT_OS "netbsd" CACHE STRING "")
set(WT_POSIX ON CACHE BOOL "")

# NetBSD requires buffers aligned to 512-byte (DEV_BSIZE) boundaries for O_DIRECT to work.
set(WT_BUFFER_ALIGNMENT_DEFAULT "512" CACHE STRING "")
