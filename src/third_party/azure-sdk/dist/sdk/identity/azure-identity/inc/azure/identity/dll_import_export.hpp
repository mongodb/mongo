// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief DLL export macro.
 */

// For explanation, see the comment in azure/core/dll_import_export.hpp

#pragma once

/**
 * @def AZ_IDENTITY_DLLEXPORT
 * @brief Applies DLL export attribute, when applicable.
 * @note See https://learn.microsoft.com/cpp/cpp/dllexport-dllimport?view=msvc-160.
 */

#if defined(AZ_IDENTITY_DLL) || (0 /*@AZ_IDENTITY_DLL_INSTALLED_AS_PACKAGE@*/)
#define AZ_IDENTITY_BUILT_AS_DLL 1
#else
#define AZ_IDENTITY_BUILT_AS_DLL 0
#endif

#if AZ_IDENTITY_BUILT_AS_DLL
#if defined(_MSC_VER)
#if defined(AZ_IDENTITY_BEING_BUILT)
#define AZ_IDENTITY_DLLEXPORT __declspec(dllexport)
#else // !defined(AZ_IDENTITY_BEING_BUILT)
#define AZ_IDENTITY_DLLEXPORT __declspec(dllimport)
#endif // AZ_IDENTITY_BEING_BUILT
#else // !defined(_MSC_VER)
#define AZ_IDENTITY_DLLEXPORT
#endif // _MSC_VER
#else // !AZ_IDENTITY_BUILT_AS_DLL
#define AZ_IDENTITY_DLLEXPORT
#endif // AZ_IDENTITY_BUILT_AS_DLL

#undef AZ_IDENTITY_BUILT_AS_DLL

/**
 * @brief Azure SDK abstractions.
 *
 */
namespace Azure {

/**
 * @brief Azure Identity SDK abstractions.
 *
 */
namespace Identity {
}
} // namespace Azure
