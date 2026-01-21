// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Run-time type info enable or disable.
 *
 * @details When RTTI is enabled, defines a macro `AZ_IDENTITY_RTTI`. When
 * the macro is not defined, RTTI is disabled.
 *
 * @details Each library has this header file. These headers are being configured by
 * `az_rtti_setup()` CMake macro. CMake install will patch this file during installation, depending
 * on the build flags.
 */

#pragma once

/**
 * @def AZ_IDENTITY_RTTI
 * @brief A macro indicating whether the code is built with RTTI or not.
 *
 * @details `AZ_RTTI` could be defined while building the Azure SDK with CMake, however, after
 * the build is completed, that information is not preserved for the code that consumes Azure SDK
 * headers, unless the code that consumes the SDK is the part of the same build process. To address
 * this issue, CMake install would patch the header it places in the installation directory, so that
 * condition:
 * `#if defined(AZ_RTTI) || (0)`
 * becomes, effectively,
 * `#if defined(AZ_RTTI) || (0 + 1)`
 * when the library was built with RTTI support, and will make no changes to the
 * condition when it was not.
 */

#if defined(AZ_RTTI) || (0 /*@AZ_IDENTITY_RTTI@*/)
#define AZ_IDENTITY_RTTI
#endif
