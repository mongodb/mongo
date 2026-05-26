# snappy: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBSNAPPY
# Layer 2 (default policy): cmake/configs/base.cmake
# Layer 3 (user toggle):    ENABLE_SNAPPY / HAVE_BUILTIN_EXTENSION_SNAPPY

# Produces target wt::snappy when the library is available.
wt_find_library(NAME snappy
    PACKAGE Snappy TARGET Snappy::snappy
    PKGCONFIG_MODULE snappy
    HEADER snappy.h)
