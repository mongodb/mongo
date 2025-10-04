/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#ifdef _MSC_VER
//disable windows complaining about max template size.
    #pragma warning (disable : 4503)
#endif // _MSC_VER

#if defined (USE_WINDOWS_DLL_SEMANTICS) || defined (_WIN32)
    #ifdef _MSC_VER
        #pragma warning(disable : 4251)
    #endif // _MSC_VER

    #ifdef USE_IMPORT_EXPORT
        #ifdef SMITHY_EXPORTS
            #define SMITHY_API __declspec(dllexport)
        #else
            #define SMITHY_API __declspec(dllimport)
        #endif /* SMITHY_EXPORTS */
    #else
        #define SMITHY_API
    #endif // USE_IMPORT_EXPORT
#else // defined (USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32)
    #if ((__GNUC__ >= 6) || defined(__clang__)) && defined(USE_IMPORT_EXPORT) && defined(SMITHY_EXPORTS)
        #define SMITHY_API __attribute__((visibility("default")))
    #else
        #define SMITHY_API
    #endif /* __GNUC__ >= 4 || defined(__clang__) */
#endif // defined (USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32)
