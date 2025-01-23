/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

namespace Aws
{
    namespace Http
    {
        enum class HttpResponseCode;
    }
    namespace Client
    {
        template<typename ERROR_TYPE>
        class AWSError;

        enum class CoreErrors
        {
            INCOMPLETE_SIGNATURE = 0,
            INTERNAL_FAILURE = 1,
            INVALID_ACTION = 2,
            INVALID_CLIENT_TOKEN_ID = 3,
            INVALID_PARAMETER_COMBINATION = 4,
            INVALID_QUERY_PARAMETER = 5,
            INVALID_PARAMETER_VALUE = 6,
            MISSING_ACTION = 7,               // SDK should never allow
            MISSING_AUTHENTICATION_TOKEN = 8, // SDK should never allow
            MISSING_PARAMETER = 9,            // SDK should never allow
            OPT_IN_REQUIRED = 10,
            REQUEST_EXPIRED = 11,
            SERVICE_UNAVAILABLE = 12,
            THROTTLING = 13,
            VALIDATION = 14,
            ACCESS_DENIED = 15,
            RESOURCE_NOT_FOUND = 16,     // Shared with multiple services
            UNRECOGNIZED_CLIENT = 17,    // Most likely caused by an invalid access key or secret key
            MALFORMED_QUERY_STRING = 18, // Where does this come from? (cognito identity uses it)
            SLOW_DOWN = 19,
            REQUEST_TIME_TOO_SKEWED = 20,
            INVALID_SIGNATURE = 21,
            SIGNATURE_DOES_NOT_MATCH = 22,
            INVALID_ACCESS_KEY_ID = 23,
            REQUEST_TIMEOUT = 24,
            NOT_INITIALIZED = 25,
            MEMORY_ALLOCATION = 26,

            NETWORK_CONNECTION = 99, // General failure to send message to service

            // These are needed for logical reasons
            UNKNOWN = 100,                // Unknown to the SDK
            CLIENT_SIGNING_FAILURE = 101, // Client failed to sign the request
            USER_CANCELLED = 102, // User cancelled the request
            ENDPOINT_RESOLUTION_FAILURE = 103,
            SERVICE_EXTENSION_START_RANGE = 128,
            OK = -1 // No error set
        };

        /**
         * Overload ostream operator<< for CoreErrors enum class for a prettier output such as "128"
         */
        AWS_CORE_API Aws::OStream& operator<< (Aws::OStream& oStream, CoreErrors code);

        namespace CoreErrorsMapper
        {
            /**
             * Finds a CoreErrors member if possible. Otherwise, returns UNKNOWN
             */
            AWS_CORE_API AWSError<CoreErrors> GetErrorForName(const char* errorName);

            /**
             * Build the mapping between predefined exception names and Aws CoreErrors using Aws::Map.
             */
            AWS_CORE_API void InitCoreErrorsMapper();

            /**
             * Cleanup memory allocated for Aws::Map used by AWS CoreError Mapper.
             */
            AWS_CORE_API void CleanupCoreErrorsMapper();
            /**
             * Finds a CoreErrors member if possible by HTTP response code
             */
            AWS_CORE_API AWSError<CoreErrors> GetErrorForHttpResponseCode(Aws::Http::HttpResponseCode code);
        } // namespace CoreErrorsMapper
    } // namespace Client
} // namespace Aws
