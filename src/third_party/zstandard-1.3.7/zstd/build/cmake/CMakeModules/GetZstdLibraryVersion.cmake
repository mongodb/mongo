function(GetZstdLibraryVersion _header _major _minor _release)
    # Read file content
    FILE(READ ${_header} CONTENT)

    string(REGEX MATCH ".*define ZSTD_VERSION_MAJOR *([0-9]+).*define ZSTD_VERSION_MINOR *([0-9]+).*define ZSTD_VERSION_RELEASE *([0-9]+)" VERSION_REGEX "${CONTENT}")
    SET(${_major} ${CMAKE_MATCH_1} PARENT_SCOPE)
    SET(${_minor} ${CMAKE_MATCH_2} PARENT_SCOPE)
    SET(${_release} ${CMAKE_MATCH_3} PARENT_SCOPE)
endfunction()
