/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#if defined (USE_WINDOWS_DLL_SEMANTICS) || defined (_WIN32)
    #ifdef _MSC_VER
        #pragma warning(disable : 4251)
    #endif // _MSC_VER
    #ifdef USE_IMPORT_EXPORT
        #ifdef AWS_ACCESS_MANAGEMENT_EXPORTS
            #define  AWS_ACCESS_MANAGEMENT_API __declspec(dllexport)
        #else // AWS_ACCESS_MANAGEMENT_EXPORTS
            #define  AWS_ACCESS_MANAGEMENT_API __declspec(dllimport)
        #endif // AWS_ACCESS_MANAGEMENT_EXPORTS
    #else // USE_IMPORT_EXPORT
        #define AWS_ACCESS_MANAGEMENT_API
    #endif // USE_IMPORT_EXPORT
#else // defined (USE_WINDOWS_DLL_SEMANTICS) || defined (_WIN32)
    #define AWS_ACCESS_MANAGEMENT_API
#endif // defined (USE_WINDOWS_DLL_SEMANTICS) || defined (_WIN32)

