/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/Version.h>
#include <aws/core/VersionConfig.h>

namespace Aws
{
namespace Version
{
  const char* GetVersionString()
  {
    return AWS_SDK_VERSION_STRING;
  }

  unsigned GetVersionMajor()
  {
    return AWS_SDK_VERSION_MAJOR;
  }

  unsigned GetVersionMinor()
  {
    return AWS_SDK_VERSION_MINOR;
  }

  unsigned GetVersionPatch()
  {
    return AWS_SDK_VERSION_PATCH;
  }


  const char* GetCompilerVersionString()
  {
#define xstr(s) str(s)
#define str(s) #s
#if defined(_MSC_VER)
      return "MSVC#" xstr(_MSC_VER);
#elif defined(__clang__)
      return "Clang#" xstr(__clang_major__) "."  xstr(__clang_minor__) "." xstr(__clang_patchlevel__);
#elif defined(__GNUC__)
      return "GCC#" xstr(__GNUC__) "."  xstr(__GNUC_MINOR__) "." xstr(__GNUC_PATCHLEVEL__);
#else
      return "UnknownCompiler";
#endif
#undef str
#undef xstr
  }

  const char* GetCPPStandard()
  {
#define xstr(s) str(s)
#define str(s) #s
#if defined(CPP_STANDARD)
    return "C++" xstr(CPP_STANDARD);
#elif defined(__cplusplus)
    switch (__cplusplus)
    {
      case 201103L:
        return "C++11";
      case 201402L:
        return "C++14";
      case 201703L:
        return "C++17";
      case 202002L:
        return "C++20";
      case 202302L:
        return "C++23";
      default:
        return "C++" xstr(__cplusplus);
    }
#else
  return "C++Unknown";
#endif
#undef str
#undef xstr
  }
} //namespace Version
} //namespace Aws


