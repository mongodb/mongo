#ifndef AWS_CAL_EXPORTS_H
#define AWS_CAL_EXPORTS_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#if defined(AWS_C_RT_USE_WINDOWS_DLL_SEMANTICS) || defined(WIN32)
#    ifdef AWS_CAL_USE_IMPORT_EXPORT
#        ifdef AWS_CAL_EXPORTS
#            define AWS_CAL_API __declspec(dllexport)
#        else
#            define AWS_CAL_API __declspec(dllimport)
#        endif /* AWS_CAL_EXPORTS */
#    else
#        define AWS_CAL_API
#    endif /* AWS_CAL_USE_IMPORT_EXPORT */

#else /* defined (AWS_C_RT_USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32) */

#    if ((__GNUC__ >= 4) || defined(__clang__)) && defined(AWS_CAL_USE_IMPORT_EXPORT) && defined(AWS_CAL_EXPORTS)
#        define AWS_CAL_API __attribute__((visibility("default")))
#    else
#        define AWS_CAL_API
#    endif /* __GNUC__ >= 4 || defined(__clang__) */

#endif /* defined (AWS_C_RT_USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32) */

#endif /* AWS_CAL_EXPORTS_H */
