# zstd: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBZSTD
# Layer 2 (default policy): cmake/configs/base.cmake
# Layer 3 (user toggle):    ENABLE_ZSTD / HAVE_BUILTIN_EXTENSION_ZSTD

# Produces target wt::zstd when the library is available.
wt_find_library(NAME zstd
    PACKAGE zstd TARGET zstd::libzstd_shared
    PKGCONFIG_MODULE libzstd
    HEADER zstd.h)
