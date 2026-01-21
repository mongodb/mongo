// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief DLL export macro.
 */

// Everything below applies to Windows builds when building the SDK library as DLL.
// We use CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS which does most of the job for us, but static data
// members do still need to be declared as __declspec(dllimport) for the client code. (See
// https://cmake.org/cmake/help/v3.13/prop_tgt/WINDOWS_DLLEXPORT_ALL_SYMBOLS.html)
// The way it works is this: each library has its own AZ_xxx_DLLEXPORT macro, which is used as
// prefix for public(*) static variables(**), this way: class Class { public:
//   AZ_xxx_DLLEXPORT static bool IsSomething;
// }
// And also each cmake file adds a corresponding build definition:
// add_compile_build_definitions(AZ_xxx_BEING_BUILT), so it IS defined at the moment the specific
// library (xxx) is being built, and is not defined at all other times. The AZ_xxx_DLLEXPORT macro
// makes the static variable to be prefixed with dllexport attribute at a time when the (xxx) is
// being built, and all other code (other libraries, customer code) see it as dllimport attribute,
// when they include the header and consume the static variable from the (xxx) library.
// For that reason, each library should have its own AZ_xxx_DLLEXPORT macro: when we build (yyy)
// library which consumes (xxx) library, (xxx)'s symbols are dllimport, while, from (yyy)'s point of
// view, (yyy)'s symbols are dllexport. Do not reuse dll_import_export.hpp file from other
// libraries, do not reuse AZ_xxx_DLLEXPORT, AZ_xxx_BEING_BUILT, AZ_xxx_DLL, or
// AZ_xxx_BUILT_AS_DLL names.
//
// CMakeLists.txt (via az_vcpkg_export()) makes sure that during the SDK build on Windows when SDK
// is being built as a DLL, AZ_xxx_DLL is defined. It is also being propagated to any code that
// consumes Azure SDK code via CMake, i.e. anything in the build tree of the Azure SDK when building
// the entire SDK, OR if a customer code consumes SDK via fetchcontent. In case that the SDK is
// being distributed as a package, i.e. vcpkg, the install step (defined in az_vcpkg_export()) will
// take care of patching the installed header to carry the knowledge that the library was built as
// DLL (and if it was built as both static and dynamic library, there will be no collision because
// each installation has its own header installation directory).
// (/*(at)AZ_xxx_DLL_INSTALLED_AS_PACKAGE(at)*/ will be replaced with "/**/ + 1 /**/") if the SDK
// library was built as Windows DLL. CMakeLists.txt snippet to achieve all this is the following
// (***):
//
//   if(WIN32 AND BUILD_SHARED_LIBS)
//     add_compile_definitions(AZ_xxx_BEING_BUILT)
//     target_compile_definitions(azure-xxx PUBLIC AZ_xxx_DLL)
//
//     set(AZ_xxx_DLL_INSTALLED_AS_PACKAGE "*/ + 1 /*")
//     configure_file(
//       "${CMAKE_CURRENT_SOURCE_DIR}/inc/azure/xxx/dll_import_export.hpp"
//       "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}/azure/xxx/dll_import_export.hpp"
//        @ONLY)
//    unset(AZ_xxx_DLL_INSTALLED_AS_PACKAGE)
//   endif()
//
// And if the SDK is being consumed using the neither option from the above (neither cmake
// fetchcontent nor a package, but some custom build process that is unknown to us, yet uncapable of
// handling AZ_xxx_BUILT_AS_DLL correctly), there is always an option for th consumer to define
// AZ_xxx_DLL manually.
// --
// (*) - could be private, but if a public inline class member function uses it, it is effectively
// public and the export attribute should be used.
// (**) - mutable or immutable (const) static variables. But not constexprs. Constexprs don't need
// the export attribute.
// (***) - note that we condition on WIN32 (i.e. all Windows including UWP, per CMake definition),
// and not on MSVC. That's because dllimport/dllexport is potentially needed on Windows platform
// regardless of the compiler used, but GCC or Clang have different syntax for that. We don't handle
// other compilers on Windows currently, but later we may add support (see "#if defined(_MSC_VER)"
// below).

#pragma once

/**
 * @def AZ_CORE_DLLEXPORT
 * @brief Applies DLL export attribute, when applicable.
 * @note See https://docs.microsoft.com/cpp/cpp/dllexport-dllimport?view=msvc-160.
 */

#if defined(AZ_CORE_DLL) || (0 /*@AZ_CORE_DLL_INSTALLED_AS_PACKAGE@*/)
#define AZ_CORE_BUILT_AS_DLL 1
#else
#define AZ_CORE_BUILT_AS_DLL 0
#endif

#if AZ_CORE_BUILT_AS_DLL
#if defined(_MSC_VER)
#if defined(AZ_CORE_BEING_BUILT)
#define AZ_CORE_DLLEXPORT __declspec(dllexport)
#else // !defined(AZ_CORE_BEING_BUILT)
#define AZ_CORE_DLLEXPORT __declspec(dllimport)
#endif // AZ_CORE_BEING_BUILT
#else // !defined(_MSC_VER)
#define AZ_CORE_DLLEXPORT
#endif // _MSC_VER
#else // !AZ_CORE_BUILT_AS_DLL
#define AZ_CORE_DLLEXPORT
#endif // AZ_CORE_BUILT_AS_DLL

#undef AZ_CORE_BUILT_AS_DLL

/**
 * @brief Azure SDK abstractions.
 */
namespace Azure {

/**
 * @brief Abstractions commonly used by Azure SDK client libraries.
 */
namespace Core {

  /**
   * @brief Credential-related abstractions.
   */
  namespace Credentials {
  }

  /**
   * @brief Cryptography-related abstractions.
   */
  namespace Cryptography {
  }

  /**
   * @brief Diagnostics-related abstractions, such as logging.
   */
  namespace Diagnostics {
  }

  /**
   * @brief Abstractions related to HTTP transport layer.
   */
  namespace Http {

    /**
     * @brief Abstractions related to controlling the behavior of HTTP requests.
     */
    namespace Policies {
    }
  } // namespace Http

  /**
   * @brief Abstractions related to communications with Azure.
   */
  namespace IO {
  }
} // namespace Core
} // namespace Azure
