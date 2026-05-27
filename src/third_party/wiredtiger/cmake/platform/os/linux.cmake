set(WT_POSIX ON CACHE BOOL "")

# Linux requires '_GNU_SOURCE' to be defined for access to GNU/Linux extension functions
# e.g. 'pthread_setname_np'.
add_compile_definitions(_GNU_SOURCE)
