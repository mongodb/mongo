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

cmake_minimum_required(VERSION 3.10.0)

if(NOT SYNC_DIR_SRC)
    message(FATAL_ERROR "Missing a source directory to sync")
endif()

if(NOT SYNC_DIR_DST)
    message(FATAL_ERROR "Missing a destination directory to sync")
endif()

# Get the list of files in the sync directory
file(GLOB files ${SYNC_DIR_SRC}/*)
# Check each file and copy over if it has changed
foreach (sync_file IN LISTS files)
    get_filename_component(sync_file_basename ${sync_file} NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${sync_file}
        ${SYNC_DIR_DST}/${sync_file_basename}
    )
endforeach()
