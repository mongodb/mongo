# memkind: capability detection + imported target.
#
# Layer 1 (capability):     HAVE_LIBMEMKIND
# Layer 2 (default policy): cmake/configs/base.cmake (DEFAULT OFF)
# Layer 3 (user toggle):    ENABLE_MEMKIND

# Produces target wt::memkind when the library is available.
wt_find_library(NAME memkind
    HEADER memkind.h)
