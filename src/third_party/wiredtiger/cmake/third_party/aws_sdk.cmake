include(ExternalProject)
include(GNUInstallDirs)

# Don't add external project if extension is not enabled.
if(NOT ENABLE_S3)
    return()
endif()

# Download and install the AWS CPP SDK into the build directory.
ExternalProject_Add(aws-sdk
    PREFIX aws-sdk-cpp
    GIT_REPOSITORY https://github.com/aws/aws-sdk-cpp.git
    GIT_TAG 1.9.175
    CMAKE_ARGS       
        -DBUILD_SHARED_LIBS=ON
        -DBUILD_ONLY=s3-crt
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_ALWAYS FALSE
    INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install
    BUILD_BYPRODUCTS
        ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-s3-crt${CMAKE_SHARED_LIBRARY_SUFFIX}
        ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-core${CMAKE_SHARED_LIBRARY_SUFFIX}
    TEST_COMMAND ""
    UPDATE_COMMAND ""
)
ExternalProject_Get_Property(aws-sdk INSTALL_DIR)

add_library(aws-sdk::core SHARED IMPORTED)
add_library(aws-sdk::s3-crt SHARED IMPORTED)
add_library(aws-sdk::crt SHARED IMPORTED)

# Small workaround to declare the include directory under INTERFACE_INCLUDE_DIRECTORIES during the configuration phase.
file(MAKE_DIRECTORY ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR})
set_target_properties(aws-sdk::core PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-core${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}                                      
)
set_target_properties(aws-sdk::s3-crt PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-s3-crt${CMAKE_SHARED_LIBRARY_SUFFIX}
    INTERFACE_INCLUDE_DIRECTORIES ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR}
)

add_dependencies(aws-sdk::core aws-sdk)
add_dependencies(aws-sdk::s3-crt aws-sdk)
