/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Http
    {
        namespace Standard
        {
            /**
             * Simple STL representation an Http Response, implements HttpResponse.
             */
            class AWS_CORE_API StandardHttpResponse : public HttpResponse
            {
            public:
                /**
                 * Initializes an http response with the originalRequest and the response code.
                 */
                StandardHttpResponse(const std::shared_ptr<const HttpRequest>& originatingRequest) :
                    HttpResponse(originatingRequest),
                    bodyStream(originatingRequest->GetResponseStreamFactory())
                {}

                ~StandardHttpResponse() = default;

                /**
                 * Get the headers from this response
                 */
                HeaderValueCollection GetHeaders() const override;
                /**
                 * Returns true if the response contains a header by headerName
                 */
                bool HasHeader(const char* headerName) const override;
                /**
                 * Returns the value for a header at headerName if it exists.
                 */
                const Aws::String& GetHeader(const Aws::String&) const override;
                /**
                 * Gets the response body of the response.
                 */
                inline Aws::IOStream& GetResponseBody() const override { return bodyStream.GetUnderlyingStream(); }
                /**
                 * Gives full control of the memory of the ResponseBody over to the caller. At this point, it is the caller's
                 * responsibility to clean up this object.
                 */
                inline Utils::Stream::ResponseStream&& SwapResponseStreamOwnership() override { return std::move(bodyStream); }
                /**
                 * Adds a header to the http response object.
                 */
                void AddHeader(const Aws::String&, const Aws::String&) override;
                /**
                 * Add a header to the http response object, and move the value.
                 * The name can't be moved as it is converted to lower-case.
                 */
                void AddHeader(const Aws::String& headerName, Aws::String&& headerValue) override;

            private:
                StandardHttpResponse(const StandardHttpResponse&);

                Aws::Map<Aws::String, Aws::String> headerMap;
                Utils::Stream::ResponseStream bodyStream;
            };

        } // namespace Standard
    } // namespace Http
} // namespace Aws


