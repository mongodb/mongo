# zlib: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBZ
# Layer 2 (default policy): cmake/configs/base.cmake
# Layer 3 (user toggle):    ENABLE_ZLIB / HAVE_BUILTIN_EXTENSION_ZLIB

# Produces target wt::zlib when the library is available.
wt_find_library(NAME z CMAKE_TARGET zlib
    PACKAGE ZLIB TARGET ZLIB::ZLIB
    PKGCONFIG_MODULE zlib
    HEADER zlib.h)
