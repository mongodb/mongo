#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

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
    configure_file(${CMAKE_CURRENT_LIST_DIR}/wiredtiger.pc.in wiredtiger.pc @ONLY)
    install(
        FILES ${CMAKE_BINARY_DIR}/wiredtiger.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()
