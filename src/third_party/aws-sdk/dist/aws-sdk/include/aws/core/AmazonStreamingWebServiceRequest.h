/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/AmazonWebServiceRequest.h>

namespace Aws
{
    static const char* DEFAULT_CONTENT_TYPE = "binary/octet-stream";

    /**
     * High-level abstraction over AWS requests that don't have well formed payloads. GetBody() uses an underlying stream that has been set by a call to SetBody()
     * Also supports request specific headers such as in rest protocols.
     */
    class AWS_CORE_API AmazonStreamingWebServiceRequest : public AmazonWebServiceRequest
    {
    public:
        /**
         * Initializes members to defaults
         */
        AmazonStreamingWebServiceRequest() : m_contentType(DEFAULT_CONTENT_TYPE)
        {
        }

        virtual ~AmazonStreamingWebServiceRequest();

        /**
         * Get the user set stream
         */
        inline std::shared_ptr<Aws::IOStream> GetBody() const override { return m_bodyStream; }
        /**
         * Set the body stream to use for the request.
         */
        inline void SetBody(const std::shared_ptr<Aws::IOStream>& body) { m_bodyStream = body; }
        /**
         * Gets all headers that will be needed in the request. Calls GetRequestSpecificHeaders(), which is the chance for subclasses to add
         * headers from their modeled data.
         */
        inline Aws::Http::HeaderValueCollection GetHeaders() const override
        {
            auto headers = GetRequestSpecificHeaders();
            headers.insert(Aws::Http::HeaderValuePair(Aws::Http::CONTENT_TYPE_HEADER, GetContentType()));

            return headers;
        }

        /**
         * AmazonStreamingWebServiceRequest always returns true.
         */
        bool IsStreaming() const override { return true; }

        /**
         * Get the user set contentType. Defaults to binary/octet-stream
         */
        const Aws::String& GetContentType() const { return m_contentType; }
        /**
          * Set the content type.
          */
        void SetContentType(const Aws::String& contentType) { m_contentType = contentType; }

    protected:
        /**
         * Override this method to put data members from a subclass into the headers collection.
         */
        virtual Aws::Http::HeaderValueCollection GetRequestSpecificHeaders() const { return Aws::Http::HeaderValueCollection(); };

    private:
        std::shared_ptr<Aws::IOStream> m_bodyStream;
        Aws::String m_contentType;
    };

} // namespace Aws

