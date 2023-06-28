include(GNUInstallDirs)

# Library installs

# Define the wiredtiger public headers we want to export when running the install target.
install(
    FILES ${CMAKE_BINARY_DIR}/include/wiredtiger.h ${CMAKE_SOURCE_DIR}/src/include/wiredtiger_ext.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Define the wiredtiger library targets we will install.
set(wt_targets)
if(ENABLE_SHARED)
    list(APPEND wt_targets wiredtiger_shared)
endif()
if(ENABLE_STATIC)
    list(APPEND wt_targets wiredtiger_static)
endif()

# Install the wiredtiger library targets.
install(TARGETS ${wt_targets}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Create our wiredtiger pkgconfig (for POSIX builds).
if(WT_POSIX)
    # Established the link flags for private libraries used by this WiredTiger. 'Private' in this context refers
    # to libraries WT links against, but isn't exposed to using applications.
    set(private_libs)
    if(HAVE_LIBPTHREAD)
        set(private_libs "${private_libs} -lpthread")
    endif()
    if(HAVE_LIBRT)
        set(private_libs "${private_libs} -lrt")
    endif()
    if(HAVE_LIBDL)
        set(private_libs "${private_libs} -ldl")
    endif()
    if(ENABLE_MEMKIND)
        set(private_libs "${private_libs} -lmemkind")
    endif()
    if(ENABLE_TCMALLOC)
        set(private_libs "${private_libs} -ltcmalloc")
    endif()
    if(ENABLE_ANTITHESIS)
        set(private_libs "${private_libs} -lvoidstar")
    endif()
    if(HAVE_BUILTIN_EXTENSION_LZ4)
        set(private_libs "${private_libs} -llz4")
    endif()
    if(HAVE_BUILTIN_EXTENSION_SNAPPY)
        set(private_libs "${private_libs} -lsnappy")
    endif()
    if(HAVE_BUILTIN_EXTENSION_SODIUM)
        set(private_libs "${private_libs} -lsodium")
    endif()
    if(HAVE_BUILTIN_EXTENSION_ZLIB)
        set(private_libs "${private_libs} -lz")
    endif()
    if(HAVE_BUILTIN_EXTENSION_ZSTD)
        set(private_libs "${private_libs} -lzstd")
    endif()
    set(PRIVATE_PKG_LIBS "${private_libs}")
    configure_file(${CMAKE_CURRENT_LIST_DIR}/wiredtiger.pc.in wiredtiger.pc @ONLY)
    install(
        FILES ${CMAKE_BINARY_DIR}/wiredtiger.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()
