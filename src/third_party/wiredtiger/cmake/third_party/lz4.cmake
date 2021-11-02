#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

if(NOT HAVE_LIBLZ4)
    # We don't need to construct a lz4 library target.
    return()
endif()

if(TARGET wt::lz4)
    # Avoid redefining the imported library, given this file can be used as an include.
    return()
endif()

# Define the imported lz4 library target that can be subsequently linked across the build system.
# We use the double colons (::) as a convention to tell CMake that the target name is associated
# with an IMPORTED target (which allows CMake to issue a diagnostic message if the library wasn't found).
add_library(wt::lz4 SHARED IMPORTED GLOBAL)
set_target_properties(wt::lz4 PROPERTIES
    IMPORTED_LOCATION ${HAVE_LIBLZ4}
)
if (HAVE_LIBLZ4_INCLUDES)
    set_target_properties(wt::lz4 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${HAVE_LIBLZ4_INCLUDES}
    )
endif()
