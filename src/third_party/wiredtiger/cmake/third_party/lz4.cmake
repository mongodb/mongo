# lz4: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBLZ4
# Layer 2 (default policy): cmake/configs/base.cmake
# Layer 3 (user toggle):    ENABLE_LZ4 / HAVE_BUILTIN_EXTENSION_LZ4

# Produces target wt::lz4 when the library is available.
wt_find_library(NAME lz4
    PACKAGE lz4 TARGET LZ4::lz4
    PKGCONFIG_MODULE liblz4
    HEADER lz4.h)
