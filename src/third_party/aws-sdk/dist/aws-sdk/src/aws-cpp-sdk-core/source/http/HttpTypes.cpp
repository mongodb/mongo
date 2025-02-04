/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/HttpTypes.h>
#include <cassert>

using namespace Aws::Http;

namespace Aws
{
namespace Http
{

namespace HttpMethodMapper
{
const char* GetNameForHttpMethod(HttpMethod httpMethod)
{
    switch (httpMethod)
    {
        case HttpMethod::HTTP_GET:
            return "GET";
        case HttpMethod::HTTP_POST:
            return "POST";
        case HttpMethod::HTTP_DELETE:
            return "DELETE";
        case HttpMethod::HTTP_PUT:
            return "PUT";
        case HttpMethod::HTTP_HEAD:
            return "HEAD";
        case HttpMethod::HTTP_PATCH:
            return "PATCH";
        case HttpMethod::HTTP_CONNECT:
          return "CONNECT";
        case HttpMethod::HTTP_OPTIONS:
          return "OPTIONS";
        case HttpMethod::HTTP_TRACE:
          return "TRACE";
        default:
          assert(0);
          return "GET";
    }
}

} // namespace HttpMethodMapper
} // namespace Http
} // namespace Aws
