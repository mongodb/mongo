/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSXmlClient.h>
#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/auth/AWSAuthSignerProvider.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/client/AWSErrorMarshaller.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/event/EventStream.h>
#include <aws/core/utils/UUID.h>
#include <smithy/tracing/TracingUtils.h>

using namespace Aws;
using namespace Aws::Client;
using namespace Aws::Http;
using namespace Aws::Utils;
using namespace Aws::Utils::Xml;
using namespace smithy::components::tracing;

static const char AWS_XML_CLIENT_LOG_TAG[] = "AWSXmlClient";

AWSXMLClient::AWSXMLClient(const Aws::Client::ClientConfiguration& configuration,
    const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer,
    const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller) :
    BASECLASS(configuration, signer, errorMarshaller)
{
}

AWSXMLClient::AWSXMLClient(const Aws::Client::ClientConfiguration& configuration,
    const std::shared_ptr<Aws::Auth::AWSAuthSignerProvider>& signerProvider,
    const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller) :
    BASECLASS(configuration, signerProvider, errorMarshaller)
{
}

XmlOutcome AWSXMLClient::MakeRequest(const Aws::AmazonWebServiceRequest& request,
                                     const Aws::Endpoint::AWSEndpoint& endpoint,
                                     Http::HttpMethod method /* = Http::HttpMethod::HTTP_POST */,
                                     const char* signerName /* = Aws::Auth::NULL_SIGNER */,
                                     const char* signerRegionOverride /* = nullptr */,
                                     const char* signerServiceNameOverride /* = nullptr */) const
{
    const Aws::Http::URI& uri = endpoint.GetURI();
    if (endpoint.GetAttributes()) {
        signerName = endpoint.GetAttributes()->authScheme.GetName().c_str();
        if (endpoint.GetAttributes()->authScheme.GetSigningRegion()) {
            signerRegionOverride = endpoint.GetAttributes()->authScheme.GetSigningRegion()->c_str();
        }
        if (endpoint.GetAttributes()->authScheme.GetSigningRegionSet()) {
            signerRegionOverride = endpoint.GetAttributes()->authScheme.GetSigningRegionSet()->c_str();
        }
        if (endpoint.GetAttributes()->authScheme.GetSigningName()) {
            signerServiceNameOverride = endpoint.GetAttributes()->authScheme.GetSigningName()->c_str();
        }
    }
    return MakeRequest(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride);
}

XmlOutcome AWSXMLClient::MakeRequest(const Aws::Endpoint::AWSEndpoint& endpoint,
                                     const char* requestName /* = "" */,
                                     Http::HttpMethod method /* = Http::HttpMethod::HTTP_POST */,
                                     const char* signerName /* = Aws::Auth::NULL_SIGNER */,
                                     const char* signerRegionOverride /* = nullptr */,
                                     const char* signerServiceNameOverride /* = nullptr */) const
{
    const Aws::Http::URI& uri = endpoint.GetURI();
    if (endpoint.GetAttributes()) {
        signerName = endpoint.GetAttributes()->authScheme.GetName().c_str();
        if (endpoint.GetAttributes()->authScheme.GetSigningRegion()) {
            signerRegionOverride = endpoint.GetAttributes()->authScheme.GetSigningRegion()->c_str();
        }
        if (endpoint.GetAttributes()->authScheme.GetSigningRegionSet()) {
            signerRegionOverride = endpoint.GetAttributes()->authScheme.GetSigningRegionSet()->c_str();
        }
        if (endpoint.GetAttributes()->authScheme.GetSigningName()) {
            signerServiceNameOverride = endpoint.GetAttributes()->authScheme.GetSigningName()->c_str();
        }
    }
    return MakeRequest(uri, method, signerName, requestName, signerRegionOverride, signerServiceNameOverride);
}

XmlOutcome AWSXMLClient::MakeRequest(const Aws::Http::URI& uri,
    const Aws::AmazonWebServiceRequest& request,
    Http::HttpMethod method,
    const char* signerName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpOutcome(BASECLASS::AttemptExhaustively(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride));
    if (!httpOutcome.IsSuccess())
    {
        return smithy::components::tracing::TracingUtils::MakeCallWithTiming<XmlOutcome>(
            [&]() -> XmlOutcome {
                return XmlOutcome(std::move(httpOutcome));
            },
            TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    }

    if (httpOutcome.GetResult()->GetResponseBody().tellp() > 0)
    {
        return smithy::components::tracing::TracingUtils::MakeCallWithTiming<XmlOutcome>(
            [&]() -> XmlOutcome {
                XmlDocument xmlDoc = XmlDocument::CreateFromXmlStream(httpOutcome.GetResult()->GetResponseBody());

                if (!xmlDoc.WasParseSuccessful())
                {
                    AWS_LOGSTREAM_ERROR(AWS_XML_CLIENT_LOG_TAG, "Xml parsing for error failed with message " << xmlDoc.GetErrorMessage().c_str());
                    return AWSError<CoreErrors>(CoreErrors::UNKNOWN, "Xml Parse Error", xmlDoc.GetErrorMessage(), false);
                }

                return XmlOutcome(AmazonWebServiceResult<XmlDocument>(std::move(xmlDoc),
                    httpOutcome.GetResult()->GetHeaders(), httpOutcome.GetResult()->GetResponseCode()));
            },
            TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    }

    return XmlOutcome(AmazonWebServiceResult<XmlDocument>(XmlDocument(), httpOutcome.GetResult()->GetHeaders()));
}

XmlOutcome AWSXMLClient::MakeRequest(const Aws::Http::URI& uri,
    Http::HttpMethod method,
    const char* signerName,
    const char* requestName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpOutcome(BASECLASS::AttemptExhaustively(uri, method, signerName, requestName, signerRegionOverride, signerServiceNameOverride));
    if (!httpOutcome.IsSuccess())
    {
        return smithy::components::tracing::TracingUtils::MakeCallWithTiming<XmlOutcome>(
            [&]() -> XmlOutcome {
                return XmlOutcome(std::move(httpOutcome));
            },
            TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    }

    if (httpOutcome.GetResult()->GetResponseBody().tellp() > 0)
    {
        return smithy::components::tracing::TracingUtils::MakeCallWithTiming<XmlOutcome>(
            [&]() -> XmlOutcome {
                return XmlOutcome(AmazonWebServiceResult<XmlDocument>(
                    XmlDocument::CreateFromXmlStream(httpOutcome.GetResult()->GetResponseBody()),
                    httpOutcome.GetResult()->GetHeaders(), httpOutcome.GetResult()->GetResponseCode()));
            },
            TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    }

    return smithy::components::tracing::TracingUtils::MakeCallWithTiming<XmlOutcome>(
        [&]() -> XmlOutcome {
            return XmlOutcome(AmazonWebServiceResult<XmlDocument>(XmlDocument(), httpOutcome.GetResult()->GetHeaders()));
        },
        TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName}, {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
}

AWSError<CoreErrors> AWSXMLClient::BuildAWSError(const std::shared_ptr<Http::HttpResponse>& httpResponse) const
{
     AWSError<CoreErrors> error;
    if (httpResponse->HasClientError())
    {
        bool retryable = httpResponse->GetClientErrorType() == CoreErrors::NETWORK_CONNECTION ? true : false;
        error = AWSError<CoreErrors>(httpResponse->GetClientErrorType(), "", httpResponse->GetClientErrorMessage(), retryable);
    }
    else if (!httpResponse->GetResponseBody() || httpResponse->GetResponseBody().tellp() < 1)
    {
        auto responseCode = httpResponse->GetResponseCode();
        auto errorCode = AWSClient::GuessBodylessErrorType(responseCode);
        Aws::StringStream ss;
        
        ss << "No response body.";
        
        error = AWSError<CoreErrors>(errorCode, "", ss.str(), IsRetryableHttpResponseCode(responseCode));
    }
    else
    {
        // When trying to build an AWS Error from a response which is an FStream, we need to rewind the
        // file pointer back to the beginning in order to correctly read the input using the XML string iterator
        if ((httpResponse->GetResponseBody().tellp() > 0)
            && (httpResponse->GetResponseBody().tellg() > 0))
        {
            httpResponse->GetResponseBody().seekg(0);
        }

        error = GetErrorMarshaller()->Marshall(*httpResponse);
    }

    error.SetResponseHeaders(httpResponse->GetHeaders());
    error.SetResponseCode(httpResponse->GetResponseCode());
    error.SetRemoteHostIpAddress(httpResponse->GetOriginatingRequest().GetResolvedRemoteHost());
    AWS_LOGSTREAM_ERROR(AWS_XML_CLIENT_LOG_TAG, error);
    return error;
}
