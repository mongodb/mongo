include(ExternalProject)
include(GNUInstallDirs)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)

# Skip the AWS SDK build step if the extension is not enabled.
if(NOT ENABLE_S3)
    return()
endif()

config_choice(
    IMPORT_S3_SDK
    "Specify how to import the S3 SDK"
    OPTIONS
        "none;IMPORT_S3_SDK_NONE;NOT ENABLE_S3"
        "package;IMPORT_S3_SDK_PACKAGE;ENABLE_S3"
        "external;IMPORT_S3_SDK_EXTERNAL;ENABLE_S3"
)
 
if(IMPORT_S3_SDK_NONE)
    message(FATAL_ERROR "Cannot enable S3 extension without specifying an IMPORT_S3_SDK method (package, external).")
endif()

set(s3_crt_lib_location)
set(aws_core_lib_location)
set(aws_sdk_include_location)

if(IMPORT_S3_SDK_PACKAGE)
    find_package(AWSSDK REQUIRED COMPONENTS s3-crt)
    # Use the AWS provided variables to set the paths for the AWS targets.
    set(s3_crt_lib_location ${AWSSDK_LIB_DIR}/libaws-cpp-sdk-s3-crt${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(aws_core_lib_location ${AWSSDK_LIB_DIR}/libaws-cpp-sdk-core${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(aws_sdk_include_location ${AWSSDK_INCLUDE_DIR})
elseif(IMPORT_S3_SDK_EXTERNAL)
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
            # ENABLE TESTING decides whether or not to build the AWS CPP SDK with the services integration tests.
            # Alternatively you can build with testing enabled but set AUTORUN_UNIT_TESTS flag to ON/OFF to decide 
            # whether or not to run the tests. If testing is not enabled, the AUTORUN_UNIT_TESTS flag gets ignored. 
            -DENABLE_TESTING=OFF
        BUILD_ALWAYS FALSE
        INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install
        BUILD_BYPRODUCTS
            ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-s3-crt${CMAKE_SHARED_LIBRARY_SUFFIX}
            ${CMAKE_CURRENT_BINARY_DIR}/aws-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-core${CMAKE_SHARED_LIBRARY_SUFFIX}
        TEST_COMMAND ""
        UPDATE_COMMAND ""
    )
    ExternalProject_Get_Property(aws-sdk INSTALL_DIR)
    file(MAKE_DIRECTORY ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR})
    # Set the path variables to be used for the AWS targets.
    set(s3_crt_lib_location ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-s3-crt${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(aws_core_lib_location ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libaws-cpp-sdk-core${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(aws_sdk_include_location ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR})
endif()

add_library(aws-sdk::core SHARED IMPORTED)
add_library(aws-sdk::s3-crt SHARED IMPORTED)
add_library(aws-sdk::crt SHARED IMPORTED)

# Small workaround to declare the include directory under INTERFACE_INCLUDE_DIRECTORIES during the configuration phase.
set_target_properties(aws-sdk::core PROPERTIES
    IMPORTED_LOCATION ${aws_core_lib_location}
    INTERFACE_INCLUDE_DIRECTORIES ${aws_sdk_include_location}
)
set_target_properties(aws-sdk::s3-crt PROPERTIES
    IMPORTED_LOCATION ${s3_crt_lib_location}
    INTERFACE_INCLUDE_DIRECTORIES ${aws_sdk_include_location}
)

if (IMPORT_S3_SDK_EXTERNAL)
    add_dependencies(aws-sdk::core aws-sdk)
    add_dependencies(aws-sdk::s3-crt aws-sdk)
endif()
