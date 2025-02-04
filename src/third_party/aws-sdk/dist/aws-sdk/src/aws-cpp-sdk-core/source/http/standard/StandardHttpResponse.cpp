/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/standard/StandardHttpResponse.h>

#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/AWSMemory.h>

#include <istream>

using namespace Aws::Http;
using namespace Aws::Http::Standard;
using namespace Aws::Utils;

static const char* STANDARD_HTTP_RESPONSE_LOG_TAG = "StandardHttpResponse";

HeaderValueCollection StandardHttpResponse::GetHeaders() const
{
    HeaderValueCollection headerValueCollection;

    for (const auto & iter : headerMap)
    {
        headerValueCollection.emplace(HeaderValuePair(iter.first, iter.second));
    }

    return headerValueCollection;
}

bool StandardHttpResponse::HasHeader(const char* headerName) const
{
    return headerMap.find(StringUtils::ToLower(headerName)) != headerMap.end();
}

const Aws::String& StandardHttpResponse::GetHeader(const Aws::String& headerName) const
{
    auto foundValue = headerMap.find(StringUtils::ToLower(headerName.c_str()));
    assert(foundValue != headerMap.end());
    if (foundValue == headerMap.end()) {
        AWS_LOGSTREAM_ERROR(STANDARD_HTTP_RESPONSE_LOG_TAG, "Requested a header value for a missing header key: " << headerName);
        static const Aws::String EMPTY_STRING;
        return EMPTY_STRING;
    }
    return foundValue->second;
}

void StandardHttpResponse::AddHeader(const Aws::String& headerName, const Aws::String& headerValue)
{
    headerMap[StringUtils::ToLower(headerName.c_str())] = headerValue;
}

void StandardHttpResponse::AddHeader(const Aws::String& headerName, Aws::String&& headerValue)
{
    headerMap.emplace(StringUtils::ToLower(headerName.c_str()), std::move(headerValue));
}


