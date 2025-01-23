/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/client/RequestCompression.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/endpoint/EndpointParameter.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/endpoint/internal/AWSEndpointAttribute.h>

namespace Aws
{
    namespace Http
    {
        class URI;
    } // namespace Http

    class AmazonWebServiceRequest;

    /**
     * Closure definition for handling a retry notification. This is only for if you want to be notified that a particular request is being retried.
     */
    typedef std::function<void(const AmazonWebServiceRequest&)> RequestRetryHandler;
    typedef std::function<void(const Aws::Http::HttpRequest&)> RequestSignedHandler;

    /**
     * Base level abstraction for all modeled AWS requests
     */
    class AWS_CORE_API AmazonWebServiceRequest
    {
    public:
        /**
         * Sets up default response stream factory. initializes other pointers to nullptr.
         */
        AmazonWebServiceRequest();
        virtual ~AmazonWebServiceRequest() = default;

        /**
         * Get the payload for the request
         */
        virtual std::shared_ptr<Aws::IOStream> GetBody() const = 0;
        /**
         * Get the headers for the request
         */
        virtual Aws::Http::HeaderValueCollection GetHeaders() const = 0;
        /**
         * Get the additional user-set custom headers for the request
         */
        virtual const Aws::Http::HeaderValueCollection& GetAdditionalCustomHeaders() const;
        /**
         * Set an additional custom header value under a key. This value will overwrite any previously set or regular header.
         */
        virtual void SetAdditionalCustomHeaderValue(const Aws::String& headerName, const Aws::String& headerValue);

        /**
         * Do nothing virtual, override this to add query strings to the request
         */
        virtual void AddQueryStringParameters(Aws::Http::URI& uri) const { AWS_UNREFERENCED_PARAM(uri); }

        /**
         * Put the request to a url for later presigning. This will push the body to the url and
         * then adds the existing query string parameters as normal.
         */
        virtual void PutToPresignedUrl(Aws::Http::URI& uri) const { DumpBodyToUrl(uri); AddQueryStringParameters(uri); }

        /**
         * Defaults to false, if this is set to true, it's a streaming request, which means the payload is not well structured bits.
         */
        virtual bool IsStreaming() const { return false; }

        /**
         * Defaults to false, if this is set to true in derived class, it's an event stream request, which means the payload is consisted by multiple structured events.
         */
        inline virtual bool IsEventStreamRequest() const { return false; }
        /**
         * Defaults to true, if this is set to false, then signers, if they support body signing, will not do so
         */
        virtual bool SignBody() const { return true; }

        /**
         * Defaults to false, if a derived class returns true it indicates that the body has an embedded error.
         */
        virtual bool HasEmbeddedError(Aws::IOStream& body, const Aws::Http::HeaderValueCollection& header) const {
            (void) body;
            (void) header;
            return false;
        }

        /**
         * Defaults to false, if this is set to true, it supports chunked transfer encoding.
         */
        virtual bool IsChunked() const { return false; }

        /**
         * Register closure for request signed event.
         */
        inline virtual void SetRequestSignedHandler(const RequestSignedHandler& handler) { m_onRequestSigned = handler; }
        /**
         * Get closure for request signed event.
         */
        inline virtual const RequestSignedHandler& GetRequestSignedHandler() const { return m_onRequestSigned; }

        /**
         * Retrieves the factory for creating response streams.
         */
        const Aws::IOStreamFactory& GetResponseStreamFactory() const { return m_responseStreamFactory; }
        /**
         * Set the response stream factory.
         */
        void SetResponseStreamFactory(const Aws::IOStreamFactory& factory) { m_responseStreamFactory = factory; }

        ///@{
        /**
         * Sets the closure for headers received event.
         */
        inline virtual void SetHeadersReceivedEventHandler(const Aws::Http::HeadersReceivedEventHandler& headersReceivedEventHandler) { m_onHeadersReceived = headersReceivedEventHandler; }
        inline virtual void SetHeadersReceivedEventHandler(Aws::Http::HeadersReceivedEventHandler&& headersReceivedEventHandler) { m_onHeadersReceived = std::move(headersReceivedEventHandler); }
        ///@}

        ///@{
        /**
         * Register closure for data received event.
         */
        inline virtual void SetDataReceivedEventHandler(const Aws::Http::DataReceivedEventHandler& dataReceivedEventHandler) { m_onDataReceived = dataReceivedEventHandler; }
        inline virtual void SetDataReceivedEventHandler(Aws::Http::DataReceivedEventHandler&& dataReceivedEventHandler) { m_onDataReceived = std::move(dataReceivedEventHandler); }
        ///@}

        ///@{
        /**
         * Register closure for data sent event
         */
        inline virtual void SetDataSentEventHandler(const Aws::Http::DataSentEventHandler& dataSentEventHandler) { m_onDataSent = dataSentEventHandler; }
        inline virtual void SetDataSentEventHandler(Aws::Http::DataSentEventHandler&& dataSentEventHandler) { m_onDataSent = std::move(dataSentEventHandler); }
        ///@}

        ///@{
        /**
         * Register closure for  handling whether or not to continue a request.
         */
        inline virtual void SetContinueRequestHandler(const Aws::Http::ContinueRequestHandler& continueRequestHandler) { m_continueRequest = continueRequestHandler; }
        inline virtual void SetContinueRequestHandler(Aws::Http::ContinueRequestHandler&& continueRequestHandler) { m_continueRequest = std::move(continueRequestHandler); }
        ///@}

        ///@{
        /**
         * Register closure for notification that a request is being retried
         */
        inline virtual void SetRequestRetryHandler(const RequestRetryHandler& handler) { m_requestRetryHandler = handler; }
        inline virtual void SetRequestRetryHandler(RequestRetryHandler&& handler) { m_requestRetryHandler = std::move(handler); }
        ///@}

        /**
         * get closure for headers received event.
         */
        inline virtual const Aws::Http::HeadersReceivedEventHandler& GetHeadersReceivedEventHandler() const { return m_onHeadersReceived; }
        /**
         * get closure for data received event.
         */
        inline virtual const Aws::Http::DataReceivedEventHandler& GetDataReceivedEventHandler() const { return m_onDataReceived; }
        /**
         * get closure for data sent event
         */
        inline virtual const Aws::Http::DataSentEventHandler& GetDataSentEventHandler() const { return m_onDataSent; }
        /**
         * get closure for handling whether or not to cancel a request.
         */
        inline virtual const Aws::Http::ContinueRequestHandler& GetContinueRequestHandler() const { return m_continueRequest; }
        /**
         * get closure for notification that a request is being retried
         */
        inline virtual const RequestRetryHandler& GetRequestRetryHandler() const { return m_requestRetryHandler; }
        /**
         * If this is set to true, content-md5 needs to be computed and set on the request
         */
        inline virtual bool ShouldComputeContentMd5() const { return false; }

        inline virtual bool ShouldValidateResponseChecksum() const { return false; }

        inline virtual Aws::Vector<Aws::String> GetResponseChecksumAlgorithmNames() const { return {}; }

        inline virtual Aws::String GetChecksumAlgorithmName() const { return {}; }

        virtual const char* GetServiceRequestName() const = 0;

        inline virtual void SetServiceSpecificParameters(const std::shared_ptr<Http::ServiceSpecificParameters>& serviceSpecificParameters) const { m_serviceSpecificParameters = serviceSpecificParameters; };

        inline virtual std::shared_ptr<Http::ServiceSpecificParameters> GetServiceSpecificParameters() const { return m_serviceSpecificParameters; };

        using EndpointParameters = Aws::Vector<Aws::Endpoint::EndpointParameter>;
        virtual EndpointParameters GetEndpointContextParams() const;

        virtual Aws::Client::CompressionAlgorithm
        GetSelectedCompressionAlgorithm(Aws::Client::RequestCompressionConfig) const { return Aws::Client::CompressionAlgorithm::NONE; }

    protected:
        /**
         * Default does nothing. Override this to convert what would otherwise be the payload of the
         *  request to a query string format.
         */
        virtual void DumpBodyToUrl(Aws::Http::URI& uri) const { AWS_UNREFERENCED_PARAM(uri); }

        Aws::Http::HeaderValueCollection m_additionalCustomHeaders;
    private:
        Aws::IOStreamFactory m_responseStreamFactory;

        Aws::Http::HeadersReceivedEventHandler m_onHeadersReceived;
        Aws::Http::DataReceivedEventHandler m_onDataReceived;
        Aws::Http::DataSentEventHandler m_onDataSent;
        Aws::Http::ContinueRequestHandler m_continueRequest;
        RequestSignedHandler m_onRequestSigned;
        RequestRetryHandler m_requestRetryHandler;
        mutable std::shared_ptr<Aws::Http::ServiceSpecificParameters> m_serviceSpecificParameters;
    };

} // namespace Aws

