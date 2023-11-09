# Install script for directory: /home/ubuntu/mongo/src/third_party/abseil-cpp/dist/absl

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/base/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/algorithm/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/cleanup/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/container/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/crc/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/debugging/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/flags/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/functional/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/hash/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/log/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/memory/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/meta/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/numeric/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/profiling/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/random/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/status/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/strings/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/synchronization/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/time/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/types/cmake_install.cmake")
  include("/home/ubuntu/mongo/src/third_party/abseil-cpp/dist/scons_gen_build/absl/utility/cmake_install.cmake")

endif()

