include(GNUInstallDirs)
include(${CMAKE_SOURCE_DIR}/cmake/helpers.cmake)
cmake_minimum_required(VERSION 3.13)

config_choice(
    IMPORT_AZURE_SDK
    "Specify how to import the Azure SDK"
    OPTIONS
        "none;IMPORT_AZURE_SDK_NONE;NOT ENABLE_AZURE"
        "package;IMPORT_AZURE_SDK_PACKAGE;ENABLE_AZURE"
        "external;IMPORT_AZURE_SDK_EXTERNAL;ENABLE_AZURE"
)

if(IMPORT_AZURE_SDK_NONE)
    message(FATAL_ERROR "Cannot enable the Azure extension without specifying an IMPORT_AZURE_SDK method (package, external).")
endif()

if(IMPORT_AZURE_SDK_PACKAGE)
    find_package(azure-storage-blobs-cpp CONFIG REQUIRED)
    find_package(azure-core-cpp CONFIG REQUIRED)
elseif (IMPORT_AZURE_SDK_EXTERNAL)
    ExternalProject_Add(
        azure-sdk
        PREFIX azure-sdk-cpp
        GIT_REPOSITORY      https://github.com/Azure/azure-sdk-for-cpp.git
        GIT_TAG             azure-storage-blobs_12.2.0
        CMAKE_ARGS
            -DBUILD_SHARED_LIBS=ON
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/azure-sdk-cpp/install
        BUILD_ALWAYS FALSE
        BUILD_BYPRODUCTS
            ${CMAKE_CURRENT_BINARY_DIR}/azure-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libazure-storage-blobs${CMAKE_SHARED_LIBRARY_SUFFIX}
            ${CMAKE_CURRENT_BINARY_DIR}/azure-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libazure-core${CMAKE_SHARED_LIBRARY_SUFFIX}
            ${CMAKE_CURRENT_BINARY_DIR}/azure-sdk-cpp/install/${CMAKE_INSTALL_LIBDIR}/libazure-storage-common${CMAKE_SHARED_LIBRARY_SUFFIX}
        INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/azure-sdk-cpp/install
        TEST_COMMAND ""
        UPDATE_COMMAND ""
    )
    ExternalProject_Get_Property(azure-sdk INSTALL_DIR)
    file(MAKE_DIRECTORY ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR})

    # Set the path variables to be used for the AZURE targets.
    set(azure_sdk_include_location ${INSTALL_DIR}/${CMAKE_INSTALL_INCLUDEDIR})
    set(azure_storage_lib_location ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libazure-storage-blobs${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(azure_core_lib_location ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libazure-core${CMAKE_SHARED_LIBRARY_SUFFIX})
    set(azure_storage_common_lib_location ${INSTALL_DIR}/${CMAKE_INSTALL_LIBDIR}/libazure-storage-common${CMAKE_SHARED_LIBRARY_SUFFIX})

    add_library(Azure::azure-storage-blobs SHARED IMPORTED)
    add_library(Azure::azure-core SHARED IMPORTED)
    add_library(Azure::azure-storage-common SHARED IMPORTED)

    # Declare the include directories under INTERFACE_INCLUDE_DIRECTORIES during the configuration phase
    # to set the IMPORTED_LOCATION for shared imported targets so that the linker knows where the shared
    # libraries are located to build the intermediate library.
    set_target_properties(Azure::azure-storage-blobs PROPERTIES
        IMPORTED_LOCATION ${azure_storage_lib_location}
        INTERFACE_INCLUDE_DIRECTORIES ${azure_sdk_include_location}
    )

    set_target_properties(Azure::azure-core  PROPERTIES
        IMPORTED_LOCATION ${azure_core_lib_location}
        INTERFACE_INCLUDE_DIRECTORIES ${azure_sdk_include_location}
    )

    set_target_properties(Azure::azure-storage-common PROPERTIES
        IMPORTED_LOCATION ${azure_storage_common_lib_location}
        INTERFACE_INCLUDE_DIRECTORIES ${azure_sdk_include_location}
    )
    add_dependencies(Azure::azure-storage-blobs azure-sdk)
    add_dependencies(Azure::azure-core azure-sdk)
endif()
