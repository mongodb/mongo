/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSList.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>

#include <memory>

namespace Aws
{
    namespace Http
    {
        /**
         * Models Http methods.
         */
    enum class HttpMethod {
      HTTP_GET,
      HTTP_POST,
      HTTP_DELETE,
      HTTP_PUT,
      HTTP_HEAD,
      HTTP_PATCH,
      HTTP_CONNECT,
      HTTP_OPTIONS,
      HTTP_TRACE,
    };

    /**
     * Possible default http factory vended http client implementations.
     */
    enum class TransferLibType { DEFAULT_CLIENT = 0, CURL_CLIENT, WIN_INET_CLIENT, WIN_HTTP_CLIENT };

    /**
     * Configuration for a HTTP client, currently used only by libCurl
     */
    enum class TransferLibPerformanceMode {
      LOW_LATENCY = 0,  // run http client for a lower latency, at the expense of CPU
      REGULAR
    };

    namespace HttpMethodMapper {
    /**
     * Gets the string value of an httpMethod.
     */
    AWS_CORE_API const char* GetNameForHttpMethod(HttpMethod httpMethod);
    }  // namespace HttpMethodMapper

        typedef std::pair<Aws::String, Aws::String> HeaderValuePair;
        typedef Aws::Map<Aws::String, Aws::String> HeaderValueCollection;

    } // namespace Http
} // namespace Aws

