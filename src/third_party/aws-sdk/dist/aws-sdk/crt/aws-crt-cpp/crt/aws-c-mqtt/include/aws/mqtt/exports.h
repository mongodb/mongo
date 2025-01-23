#ifndef AWS_MQTT_EXPORTS_H
#define AWS_MQTT_EXPORTS_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#if defined(USE_WINDOWS_DLL_SEMANTICS) || defined(WIN32)
#    ifdef AWS_MQTT_USE_IMPORT_EXPORT
#        ifdef AWS_MQTT_EXPORTS
#            define AWS_MQTT_API __declspec(dllexport)
#        else
#            define AWS_MQTT_API __declspec(dllimport)
#        endif /* AWS_MQTT_EXPORTS */
#    else
#        define AWS_MQTT_API
#    endif /* USE_IMPORT_EXPORT */

#else /* defined (USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32) */
#    if ((__GNUC__ >= 4) || defined(__clang__)) && defined(AWS_MQTT_USE_IMPORT_EXPORT) && defined(AWS_MQTT_EXPORTS)
#        define AWS_MQTT_API __attribute__((visibility("default")))
#    else
#        define AWS_MQTT_API
#    endif /* __GNUC__ >= 4 || defined(__clang__) */

#endif /* defined (USE_WINDOWS_DLL_SEMANTICS) || defined (WIN32) */

#endif /* AWS_MQTT_EXPORTS_H */
