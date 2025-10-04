include(CMakeFindDependencyMacro)
find_dependency(Threads)  # Required for Threads::Threads

# Import common targets first:
include("${CMAKE_CURRENT_LIST_DIR}/bson-targets.cmake")

# Now import the targets of each link-kind that's available. Only the targets of
# libbson libraries that were actually installed alongside this file will be
# imported.
file(GLOB target_files "${CMAKE_CURRENT_LIST_DIR}/bson_*-targets.cmake")
foreach(inc IN LISTS target_files)
    include("${inc}")
endforeach()
