# libsodium: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBSODIUM
# Layer 2 (default policy): cmake/configs/base.cmake (DEFAULT OFF)
# Layer 3 (user toggle):    ENABLE_SODIUM / HAVE_BUILTIN_EXTENSION_SODIUM

# Produces target wt::sodium when the library is available.
wt_find_library(NAME sodium
    PKGCONFIG_MODULE libsodium
    HEADER sodium.h)
