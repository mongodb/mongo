#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#  All rights reserved.
#
#  See the file LICENSE for redistribution information
#

include(GNUInstallDirs)

# Library installs

# Define the public headers for wiredtiger library to be used when installing the target.
set_property(
    TARGET wiredtiger
    PROPERTY PUBLIC_HEADER
    ${CMAKE_BINARY_DIR}/include/wiredtiger.h
    ${CMAKE_SOURCE_DIR}/src/include/wiredtiger_ext.h
)
# Set the version property of the wiredtiger library so we can export a versioned install.
set_target_properties(wiredtiger PROPERTIES VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

# Install the wiredtiger library target.
install(TARGETS wiredtiger
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Create our wiredtiger pkgconfig (for POSIX builds).
if(WT_POSIX)
    configure_file(${CMAKE_CURRENT_LIST_DIR}/wiredtiger.pc.in wiredtiger.pc @ONLY)
    install(
        FILES ${CMAKE_BINARY_DIR}/wiredtiger.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()
