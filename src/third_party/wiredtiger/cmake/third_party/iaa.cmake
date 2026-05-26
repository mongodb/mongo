# Intel IAA / QPL + libaccel-config: capability detection + imported targets.
#
# Layer 1 (capability):     HAVE_LIBQPL, HAVE_LIBACCEL_CONFIG
# Layer 2 (default policy): cmake/configs/base.cmake
# Layer 3 (user toggle):    ENABLE_IAA / HAVE_BUILTIN_EXTENSION_IAA

# Produces target wt::qpl when the library is available.
wt_find_library(NAME qpl
    HEADER qpl/qpl.h)

# Produces target wt::accel_config when the library is available.
wt_find_library(NAME accel_config
    LIBRARY accel-config)
