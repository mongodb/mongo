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
