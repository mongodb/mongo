# This is a basic version file for the Config-mode of find_package().
# It is used by write_basic_package_version_file() as input file for configure_file()
# to create a version-file which can be installed along a config.cmake file.
#
# The created file sets PACKAGE_VERSION_EXACT if the current version string and
# the requested version string are exactly the same and it sets
# PACKAGE_VERSION_COMPATIBLE if the current version is equal to the requested version.
# The tweak version component is ignored.
# The variable CVF_VERSION must be set before calling configure_file().


if (PACKAGE_FIND_VERSION_RANGE)
  message(AUTHOR_WARNING
    "`find_package()` specify a version range but the version strategy "
    "(ExactVersion) of the module `${PACKAGE_FIND_NAME}` is incompatible "
    "with this request. Only the lower endpoint of the range will be used.")
endif()

set(PACKAGE_VERSION "20230802")

if("20230802" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)") # strip the tweak version
  set(CVF_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(CVF_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(CVF_VERSION_PATCH "${CMAKE_MATCH_3}")

  if(NOT CVF_VERSION_MAJOR VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" CVF_VERSION_MAJOR "${CVF_VERSION_MAJOR}")
  endif()
  if(NOT CVF_VERSION_MINOR VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" CVF_VERSION_MINOR "${CVF_VERSION_MINOR}")
  endif()
  if(NOT CVF_VERSION_PATCH VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" CVF_VERSION_PATCH "${CVF_VERSION_PATCH}")
  endif()

  set(CVF_VERSION_NO_TWEAK "${CVF_VERSION_MAJOR}.${CVF_VERSION_MINOR}.${CVF_VERSION_PATCH}")
else()
  set(CVF_VERSION_NO_TWEAK "20230802")
endif()

if(PACKAGE_FIND_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)") # strip the tweak version
  set(REQUESTED_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(REQUESTED_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(REQUESTED_VERSION_PATCH "${CMAKE_MATCH_3}")

  if(NOT REQUESTED_VERSION_MAJOR VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" REQUESTED_VERSION_MAJOR "${REQUESTED_VERSION_MAJOR}")
  endif()
  if(NOT REQUESTED_VERSION_MINOR VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" REQUESTED_VERSION_MINOR "${REQUESTED_VERSION_MINOR}")
  endif()
  if(NOT REQUESTED_VERSION_PATCH VERSION_EQUAL 0)
    string(REGEX REPLACE "^0+" "" REQUESTED_VERSION_PATCH "${REQUESTED_VERSION_PATCH}")
  endif()

  set(REQUESTED_VERSION_NO_TWEAK
      "${REQUESTED_VERSION_MAJOR}.${REQUESTED_VERSION_MINOR}.${REQUESTED_VERSION_PATCH}")
else()
  set(REQUESTED_VERSION_NO_TWEAK "${PACKAGE_FIND_VERSION}")
endif()

if(REQUESTED_VERSION_NO_TWEAK STREQUAL CVF_VERSION_NO_TWEAK)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()

if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
endif()


# if the installed project requested no architecture check, don't perform the check
if("FALSE")
  return()
endif()

# if the installed or the using project don't have CMAKE_SIZEOF_VOID_P set, ignore it:
if("${CMAKE_SIZEOF_VOID_P}" STREQUAL "" OR "8" STREQUAL "")
  return()
endif()

# check that the installed version has the same 32/64bit-ness as the one which is currently searching:
if(NOT CMAKE_SIZEOF_VOID_P STREQUAL "8")
  math(EXPR installedBits "8 * 8")
  set(PACKAGE_VERSION "${PACKAGE_VERSION} (${installedBits}bit)")
  set(PACKAGE_VERSION_UNSUITABLE TRUE)
endif()
