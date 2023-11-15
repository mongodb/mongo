# Move load_gdb_scripts.py and its related gdb scripts into the build directory. 
# Rename the loader script to allow gdb to execute it automatically when the .so file is loaded.
function(setup_gdb_autoloader)
    if(WT_LINUX AND ENABLE_SHARED)
        message(STATUS "Setting up gdb script auto-loading.")
        add_custom_target(copy-autoload-script ALL
            COMMAND ${CMAKE_COMMAND} -E copy 
                ${CMAKE_SOURCE_DIR}/tools/gdb/load_gdb_scripts.py 
                ${CMAKE_BINARY_DIR}/libwiredtiger.so.${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}-gdb.py
        )

        add_custom_target(copy-runtime-files ALL
            COMMAND ${CMAKE_COMMAND} -E copy_directory 
            ${CMAKE_SOURCE_DIR}/tools/gdb/gdb_scripts 
            ${CMAKE_BINARY_DIR}/gdb_scripts
        )
    else()
        # The auto-loading process requires we've created a WiredTiger.so library and that gdb 
        # is available on the system. For now assume only Linux hosts have gdb.
        message(STATUS "NOT setting up gdb script auto-loading.")
    endif()
endfunction()
