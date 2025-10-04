/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/AWSClient.h>
#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/core/auth/AWSAuthSignerProvider.h>
#include <aws/core/client/AWSUrlPresigner.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/client/AWSErrorMarshaller.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/client/RequestCompression.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/stream/ResponseStream.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>
#include <aws/core/utils/crypto/MD5.h>
#include <aws/core/utils/crypto/CRC32.h>
#include <aws/core/utils/crypto/Sha256.h>
#include <aws/core/utils/crypto/Sha1.h>
#include <aws/core/utils/crypto/PrecalculatedHash.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/event/EventStream.h>
#include <aws/core/utils/UUID.h>
#include <aws/core/monitoring/MonitoringManager.h>
#include <aws/core/Region.h>
#include <aws/core/utils/DNS.h>
#include <aws/core/Version.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/platform/OSVersionInfo.h>

#include <smithy/tracing/TracingUtils.h>
#include <smithy/client/features/ChecksumInterceptor.h>

#include <cstring>
#include <cassert>
#include <iomanip>

using namespace Aws;
using namespace Aws::Client;
using namespace Aws::Http;
using namespace Aws::Utils;
using namespace Aws::Utils::Json;
using namespace Aws::Utils::Xml;
using namespace smithy::components::tracing;
using namespace smithy::interceptor;

static const int SUCCESS_RESPONSE_MIN = 200;
static const int SUCCESS_RESPONSE_MAX = 299;

static const char AWS_CLIENT_LOG_TAG[] = "AWSClient";
static const char AWS_LAMBDA_FUNCTION_NAME[] = "AWS_LAMBDA_FUNCTION_NAME";
static const char X_AMZN_TRACE_ID[] = "_X_AMZN_TRACE_ID";

//4 Minutes
static const std::chrono::milliseconds TIME_DIFF_MAX = std::chrono::minutes(4);
//-4 Minutes
static const std::chrono::milliseconds TIME_DIFF_MIN = std::chrono::minutes(-4);

CoreErrors AWSClient::GuessBodylessErrorType(Aws::Http::HttpResponseCode responseCode)
{
    switch (responseCode)
    {
    case HttpResponseCode::FORBIDDEN:
    case HttpResponseCode::UNAUTHORIZED:
        return CoreErrors::ACCESS_DENIED;
    case HttpResponseCode::NOT_FOUND:
        return CoreErrors::RESOURCE_NOT_FOUND;
    default:
        return CoreErrors::UNKNOWN;
    }
}

bool AWSClient::DoesResponseGenerateError(const std::shared_ptr<HttpResponse>& response)
{
    if (response->HasClientError()) return true;

    int responseCode = static_cast<int>(response->GetResponseCode());
    return responseCode < SUCCESS_RESPONSE_MIN || responseCode > SUCCESS_RESPONSE_MAX;
}

struct RequestInfo
{
    Aws::Utils::DateTime ttl;
    long attempt;
    long maxAttempts;

    operator String()
    {
        Aws::StringStream ss;
        if (ttl.WasParseSuccessful() && ttl != DateTime())
        {
            assert(attempt > 1);
            ss << "ttl=" << ttl.ToGmtString(DateFormat::ISO_8601_BASIC) << "; ";
        }
        ss << "attempt=" << attempt;
        if (maxAttempts > 0)
        {
            ss << "; max=" << maxAttempts;
        }
        return ss.str();
    }
};

AWSClient::AWSClient(const Aws::Client::ClientConfiguration& configuration,
    const std::shared_ptr<Aws::Client::AWSAuthSigner>& signer,
    const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller) :
    m_region(configuration.region),
    m_telemetryProvider(configuration.telemetryProvider ? configuration.telemetryProvider : configuration.configFactories.telemetryProviderCreateFn()),
    m_signerProvider(Aws::MakeUnique<Aws::Auth::DefaultAuthSignerProvider>(AWS_CLIENT_LOG_TAG, signer)),
    m_httpClient(CreateHttpClient(
        [&configuration, this]()
        {
            ClientConfiguration tempConfig(configuration);
            tempConfig.telemetryProvider = m_telemetryProvider;
            return tempConfig;
        }())),
    m_errorMarshaller(errorMarshaller),
    m_retryStrategy(configuration.retryStrategy ? configuration.retryStrategy : configuration.configFactories.retryStrategyCreateFn()),
    m_writeRateLimiter(configuration.writeRateLimiter ? configuration.writeRateLimiter : configuration.configFactories.writeRateLimiterCreateFn()),
    m_readRateLimiter(configuration.readRateLimiter ? configuration.readRateLimiter : configuration.configFactories.readRateLimiterCreateFn()),
    m_userAgent(Aws::Client::ComputeUserAgentString(&configuration)),
    m_hash(Aws::Utils::Crypto::CreateMD5Implementation()),
    m_requestTimeoutMs(configuration.requestTimeoutMs),
    m_enableClockSkewAdjustment(configuration.enableClockSkewAdjustment),
    m_requestCompressionConfig(configuration.requestCompressionConfig),
    m_interceptors{Aws::MakeShared<smithy::client::ChecksumInterceptor>(AWS_CLIENT_LOG_TAG)}
{
}

AWSClient::AWSClient(const Aws::Client::ClientConfiguration& configuration,
    const std::shared_ptr<Aws::Auth::AWSAuthSignerProvider>& signerProvider,
    const std::shared_ptr<AWSErrorMarshaller>& errorMarshaller) :
    m_region(configuration.region),
    m_telemetryProvider(configuration.telemetryProvider ? configuration.telemetryProvider : configuration.configFactories.telemetryProviderCreateFn()),
    m_signerProvider(signerProvider),
    m_httpClient(CreateHttpClient(
        [&configuration, this]()
        {
            ClientConfiguration tempConfig(configuration);
            tempConfig.telemetryProvider = m_telemetryProvider;
            return tempConfig;
        }())),
    m_errorMarshaller(errorMarshaller),
    m_retryStrategy(configuration.retryStrategy ? configuration.retryStrategy : configuration.configFactories.retryStrategyCreateFn()),
    m_writeRateLimiter(configuration.writeRateLimiter ? configuration.writeRateLimiter : configuration.configFactories.writeRateLimiterCreateFn()),
    m_readRateLimiter(configuration.readRateLimiter ? configuration.readRateLimiter : configuration.configFactories.readRateLimiterCreateFn()),
    m_userAgent(Aws::Client::ComputeUserAgentString(&configuration)),
    m_hash(Aws::Utils::Crypto::CreateMD5Implementation()),
    m_requestTimeoutMs(configuration.requestTimeoutMs),
    m_enableClockSkewAdjustment(configuration.enableClockSkewAdjustment),
    m_requestCompressionConfig(configuration.requestCompressionConfig)
{
    m_interceptors.emplace_back(Aws::MakeUnique<smithy::client::ChecksumInterceptor>(AWS_CLIENT_LOG_TAG));
}

void AWSClient::DisableRequestProcessing()
{
    m_httpClient->DisableRequestProcessing();
}

void AWSClient::EnableRequestProcessing()
{
    m_httpClient->EnableRequestProcessing();
}

void AWSClient::SetServiceClientName(const Aws::String& name)
{
    m_serviceName = std::move(name);
    AppendToUserAgent("api/" + m_serviceName);
}

void AWSClient::AppendToUserAgent(const Aws::String& valueToAppend)
{
    Aws::String value = Aws::Client::FilterUserAgentToken(valueToAppend.c_str());
    if (value.empty())
        return;
    if (m_userAgent.find(value) != Aws::String::npos)
        return;
    m_userAgent += " " + std::move(value);
}

Aws::Client::AWSAuthSigner* AWSClient::GetSignerByName(const char* name) const
{
    const auto& signer =  m_signerProvider->GetSigner(name);
    return signer ? signer.get() : nullptr;
}

static DateTime GetServerTimeFromError(const AWSError<CoreErrors> error)
{
    const Http::HeaderValueCollection& headers = error.GetResponseHeaders();
    auto awsDateHeaderIter = headers.find(StringUtils::ToLower(Http::AWS_DATE_HEADER));
    auto dateHeaderIter = headers.find(StringUtils::ToLower(Http::DATE_HEADER));
    if (awsDateHeaderIter != headers.end())
    {
        return DateTime(awsDateHeaderIter->second.c_str(), DateFormat::AutoDetect);
    }
    else if (dateHeaderIter != headers.end())
    {
        return DateTime(dateHeaderIter->second.c_str(), DateFormat::AutoDetect);
    }
    else
    {
        return DateTime();
    }
}

bool AWSClient::AdjustClockSkew(HttpResponseOutcome& outcome, const char* signerName) const
{
    if (m_enableClockSkewAdjustment)
    {
        auto signer = GetSignerByName(signerName);
        //detect clock skew and try to correct.
        AWS_LOGSTREAM_WARN(AWS_CLIENT_LOG_TAG, "If the signature check failed. This could be because of a time skew. Attempting to adjust the signer.");

        DateTime serverTime = GetServerTimeFromError(outcome.GetError());
        const auto signingTimestamp = signer->GetSigningTimestamp();
        if (!serverTime.WasParseSuccessful() || serverTime == DateTime())
        {
            AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Date header was not found in the response, can't attempt to detect clock skew");
            return false;
        }

        AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Server time is " << serverTime.ToGmtString(DateFormat::RFC822) << ", while client time is " << DateTime::Now().ToGmtString(DateFormat::RFC822));
        auto diff = DateTime::Diff(serverTime, signingTimestamp);
        //only try again if clock skew was the cause of the error.
        if (diff >= TIME_DIFF_MAX || diff <= TIME_DIFF_MIN)
        {
            diff = DateTime::Diff(serverTime, DateTime::Now());
            AWS_LOGSTREAM_INFO(AWS_CLIENT_LOG_TAG, "Computed time difference as " << diff.count() << " milliseconds. Adjusting signer with the skew.");
            signer->SetClockSkew(diff);
            AWSError<CoreErrors> newError(
                outcome.GetError().GetErrorType(), outcome.GetError().GetExceptionName(), outcome.GetError().GetMessage(), true);
            newError.SetResponseHeaders(outcome.GetError().GetResponseHeaders());
            newError.SetResponseCode(outcome.GetError().GetResponseCode());
            outcome = std::move(newError);
            return true;
        }
    }
    return false;
}

HttpResponseOutcome AWSClient::AttemptExhaustively(const Aws::Http::URI& uri,
    const Aws::AmazonWebServiceRequest& request,
    Aws::Http::HttpMethod method,
    const char* signerName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    if (!Aws::Utils::IsValidHost(uri.GetAuthority()))
    {
        return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::VALIDATION, "", "Invalid DNS Label found in URI host", false/*retryable*/));
    }
    std::shared_ptr<HttpRequest> httpRequest(CreateHttpRequest(uri, method, request.GetResponseStreamFactory()));
    HttpResponseOutcome outcome;
    AWSError<CoreErrors> lastError;
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    auto contexts = Aws::Monitoring::OnRequestStarted(this->GetServiceClientName(), request.GetServiceRequestName(), httpRequest);
    const char* signerRegion = signerRegionOverride;
    Aws::String regionFromResponse;

    Aws::String invocationId = Aws::Utils::UUID::PseudoRandomUUID();
    RequestInfo requestInfo;
    requestInfo.attempt = 1;
    requestInfo.maxAttempts = 0;
    httpRequest->SetHeaderValue(Http::SDK_INVOCATION_ID_HEADER, invocationId);
    httpRequest->SetHeaderValue(Http::SDK_REQUEST_HEADER, requestInfo);
    AppendRecursionDetectionHeader(httpRequest);

    for (long retries = 0;; retries++)
    {
        if(!m_retryStrategy->HasSendToken())
        {
            return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::SLOW_DOWN,
                                                               "",
                                                               "Unable to acquire enough send tokens to execute request.",
                                                               false/*retryable*/));

        };
        httpRequest->SetEventStreamRequest(request.IsEventStreamRequest());

        outcome = AttemptOneRequest(httpRequest, request, signerName, signerRegion, signerServiceNameOverride);
        outcome.SetRetryCount(retries);
        if (retries == 0)
        {
            m_retryStrategy->RequestBookkeeping(outcome);
        }
        else
        {
            m_retryStrategy->RequestBookkeeping(outcome, lastError);
        }
        coreMetrics.httpClientMetrics = httpRequest->GetRequestMetrics();
        TracingUtils::EmitCoreHttpMetrics(httpRequest->GetRequestMetrics(),
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        if (outcome.IsSuccess())
        {
            Aws::Monitoring::OnRequestSucceeded(this->GetServiceClientName(), request.GetServiceRequestName(), httpRequest, outcome, coreMetrics, contexts);
            AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Request successful returning.");
            break;
        }
        lastError = outcome.GetError();

        DateTime serverTime = GetServerTimeFromError(outcome.GetError());
        auto clockSkew = DateTime::Diff(serverTime, DateTime::Now());

        Aws::Monitoring::OnRequestFailed(this->GetServiceClientName(), request.GetServiceRequestName(), httpRequest, outcome, coreMetrics, contexts);

        if (!m_httpClient->IsRequestProcessingEnabled())
        {
            AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Request was cancelled externally.");
            break;
        }

        // Adjust region
        bool retryWithCorrectRegion = false;
        HttpResponseCode httpResponseCode = outcome.GetError().GetResponseCode();
        if (httpResponseCode == HttpResponseCode::MOVED_PERMANENTLY ||  // 301
            httpResponseCode == HttpResponseCode::TEMPORARY_REDIRECT || // 307
            httpResponseCode == HttpResponseCode::BAD_REQUEST ||        // 400
            httpResponseCode == HttpResponseCode::FORBIDDEN)            // 403
        {
            regionFromResponse = GetErrorMarshaller()->ExtractRegion(outcome.GetError());
            if (m_region == Aws::Region::AWS_GLOBAL && !regionFromResponse.empty() && regionFromResponse != signerRegion)
            {
                signerRegion = regionFromResponse.c_str();
                retryWithCorrectRegion = true;
            }
        }

        long sleepMillis = TracingUtils::MakeCallWithTiming<long>(
            [&]() -> long {
                return m_retryStrategy->CalculateDelayBeforeNextRetry(outcome.GetError(), retries);
            },
            TracingUtils::SMITHY_CLIENT_SERVICE_BACKOFF_DELAY_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        //AdjustClockSkew returns true means clock skew was the problem and skew was adjusted, false otherwise.
        //sleep if clock skew and region was NOT the problem. AdjustClockSkew may update error inside outcome.
        bool shouldSleep = !AdjustClockSkew(outcome, signerName) && !retryWithCorrectRegion;

        if (!retryWithCorrectRegion && !m_retryStrategy->ShouldRetry(outcome.GetError(), retries))
        {
            break;
        }

        AWS_LOGSTREAM_WARN(AWS_CLIENT_LOG_TAG, "Request failed, now waiting " << sleepMillis << " ms before attempting again.");
        if(request.GetBody())
        {
            request.GetBody()->clear();
            request.GetBody()->seekg(0);
        }

        if (request.GetRequestRetryHandler())
        {
            request.GetRequestRetryHandler()(request);
        }

        if (shouldSleep)
        {
            m_httpClient->RetryRequestSleep(std::chrono::milliseconds(sleepMillis));
        }

        Aws::Http::URI newUri = uri;
        Aws::String newEndpoint = GetErrorMarshaller()->ExtractEndpoint(outcome.GetError());
        if (!newEndpoint.empty())
        {
            newUri.SetAuthority(newEndpoint);
        }
        httpRequest = CreateHttpRequest(newUri, method, request.GetResponseStreamFactory());

        httpRequest->SetHeaderValue(Http::SDK_INVOCATION_ID_HEADER, invocationId);
        if (serverTime.WasParseSuccessful() && serverTime != DateTime())
        {
            requestInfo.ttl = DateTime::Now() + clockSkew + std::chrono::milliseconds(m_requestTimeoutMs);
        }
        requestInfo.attempt ++;
        requestInfo.maxAttempts = m_retryStrategy->GetMaxAttempts();
        httpRequest->SetHeaderValue(Http::SDK_REQUEST_HEADER, requestInfo);
        Aws::Monitoring::OnRequestRetry(this->GetServiceClientName(), request.GetServiceRequestName(), httpRequest, contexts);
    }
    auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
    auto counter = meter->CreateCounter(TracingUtils::SMITHY_CLIENT_SERVICE_ATTEMPTS_METRIC, TracingUtils::COUNT_METRIC_TYPE, "");
    counter->add(requestInfo.attempt, {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    Aws::Monitoring::OnFinish(this->GetServiceClientName(), request.GetServiceRequestName(), httpRequest, contexts);
    return outcome;
}

HttpResponseOutcome AWSClient::AttemptExhaustively(const Aws::Http::URI& uri,
    Aws::Http::HttpMethod method,
    const char* signerName,
    const char* requestName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    if (!Aws::Utils::IsValidHost(uri.GetAuthority()))
    {
        return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::VALIDATION, "", "Invalid DNS Label found in URI host", false/*retryable*/));
    }

    std::shared_ptr<HttpRequest> httpRequest(CreateHttpRequest(uri, method, Aws::Utils::Stream::DefaultResponseStreamFactoryMethod));
    HttpResponseOutcome outcome;
    AWSError<CoreErrors> lastError;
    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    auto contexts = Aws::Monitoring::OnRequestStarted(this->GetServiceClientName(), requestName, httpRequest);
    const char* signerRegion = signerRegionOverride;
    Aws::String regionFromResponse;

    Aws::String invocationId = Aws::Utils::UUID::PseudoRandomUUID();
    RequestInfo requestInfo;
    requestInfo.attempt = 1;
    requestInfo.maxAttempts = 0;
    httpRequest->SetHeaderValue(Http::SDK_INVOCATION_ID_HEADER, invocationId);
    httpRequest->SetHeaderValue(Http::SDK_REQUEST_HEADER, requestInfo);
    AppendRecursionDetectionHeader(httpRequest);

    for (long retries = 0;; retries++)
    {
        if(!m_retryStrategy->HasSendToken())
        {
            return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::SLOW_DOWN,
                                                            "",
                                                            "Unable to acquire enough send tokens to execute request.",
                                                            false/*retryable*/));

        };
        outcome = AttemptOneRequest(httpRequest, signerName, requestName, signerRegion, signerServiceNameOverride);
        outcome.SetRetryCount(retries);
        if (retries == 0)
        {
            m_retryStrategy->RequestBookkeeping(outcome);
        }
        else
        {
            m_retryStrategy->RequestBookkeeping(outcome, lastError);
        }
        coreMetrics.httpClientMetrics = httpRequest->GetRequestMetrics();
        TracingUtils::EmitCoreHttpMetrics(httpRequest->GetRequestMetrics(),
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        if (outcome.IsSuccess())
        {
            Aws::Monitoring::OnRequestSucceeded(this->GetServiceClientName(), requestName, httpRequest, outcome, coreMetrics, contexts);
            AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Request successful returning.");
            break;
        }
        lastError = outcome.GetError();

        DateTime serverTime = GetServerTimeFromError(outcome.GetError());
        auto clockSkew = DateTime::Diff(serverTime, DateTime::Now());

        Aws::Monitoring::OnRequestFailed(this->GetServiceClientName(), requestName, httpRequest, outcome, coreMetrics, contexts);

        if (!m_httpClient->IsRequestProcessingEnabled())
        {
            AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Request was cancelled externally.");
            break;
        }

        // Adjust region
        bool retryWithCorrectRegion = false;
        HttpResponseCode httpResponseCode = outcome.GetError().GetResponseCode();
        if (httpResponseCode == HttpResponseCode::MOVED_PERMANENTLY ||  // 301
            httpResponseCode == HttpResponseCode::TEMPORARY_REDIRECT || // 307
            httpResponseCode == HttpResponseCode::BAD_REQUEST ||        // 400
            httpResponseCode == HttpResponseCode::FORBIDDEN)            // 403
        {
            regionFromResponse = GetErrorMarshaller()->ExtractRegion(outcome.GetError());
            if (m_region == Aws::Region::AWS_GLOBAL && !regionFromResponse.empty() && regionFromResponse != signerRegion)
            {
                signerRegion = regionFromResponse.c_str();
                retryWithCorrectRegion = true;
            }
        }

        long sleepMillis = TracingUtils::MakeCallWithTiming<long>(
            [&]() -> long {
                return m_retryStrategy->CalculateDelayBeforeNextRetry(outcome.GetError(), retries);
            },
            TracingUtils::SMITHY_CLIENT_SERVICE_BACKOFF_DELAY_METRIC,
            *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        //AdjustClockSkew returns true means clock skew was the problem and skew was adjusted, false otherwise.
        //sleep if clock skew and region was NOT the problem. AdjustClockSkew may update error inside outcome.
        bool shouldSleep = !AdjustClockSkew(outcome, signerName) && !retryWithCorrectRegion;

        if (!retryWithCorrectRegion && !m_retryStrategy->ShouldRetry(outcome.GetError(), retries))
        {
            break;
        }

        AWS_LOGSTREAM_WARN(AWS_CLIENT_LOG_TAG, "Request failed, now waiting " << sleepMillis << " ms before attempting again.");

        if (shouldSleep)
        {
            m_httpClient->RetryRequestSleep(std::chrono::milliseconds(sleepMillis));
        }

        Aws::Http::URI newUri = uri;
        Aws::String newEndpoint = GetErrorMarshaller()->ExtractEndpoint(outcome.GetError());
        if (!newEndpoint.empty())
        {
            newUri.SetAuthority(newEndpoint);
        }
        httpRequest = CreateHttpRequest(newUri, method, Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);

        httpRequest->SetHeaderValue(Http::SDK_INVOCATION_ID_HEADER, invocationId);
        if (serverTime.WasParseSuccessful() && serverTime != DateTime())
        {
            requestInfo.ttl = DateTime::Now() + clockSkew + std::chrono::milliseconds(m_requestTimeoutMs);
        }
        requestInfo.attempt ++;
        requestInfo.maxAttempts = m_retryStrategy->GetMaxAttempts();
        httpRequest->SetHeaderValue(Http::SDK_REQUEST_HEADER, requestInfo);
        Aws::Monitoring::OnRequestRetry(this->GetServiceClientName(), requestName, httpRequest, contexts);
    }
    auto meter = m_telemetryProvider->getMeter(this->GetServiceClientName(), {});
    auto counter = meter->CreateCounter(TracingUtils::SMITHY_CLIENT_SERVICE_ATTEMPTS_METRIC, TracingUtils::COUNT_METRIC_TYPE, "");
    counter->add(requestInfo.attempt, {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    Aws::Monitoring::OnFinish(this->GetServiceClientName(), requestName, httpRequest, contexts);
    return outcome;
}

HttpResponseOutcome AWSClient::AttemptOneRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest, const Aws::AmazonWebServiceRequest& request,
    const char* signerName, const char* signerRegionOverride, const char* signerServiceNameOverride) const
{
    TracingUtils::MakeCallWithTiming(
        [&]() -> void {
            BuildHttpRequest(request, httpRequest);
        },
        TracingUtils::SMITHY_CLIENT_SERIALIZATION_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    InterceptorContext context{request};
    context.SetTransmitRequest(httpRequest);
    for (const auto& interceptor : m_interceptors)
    {
        const auto modifiedRequest = interceptor->ModifyBeforeSigning(context);
        if (!modifiedRequest.IsSuccess())
        {
            return modifiedRequest.GetError();
        }
    }

    auto signer = GetSignerByName(signerName);
    auto signedRequest = TracingUtils::MakeCallWithTiming<bool>([&]() -> bool {
            return signer->SignRequest(*httpRequest, signerRegionOverride, signerServiceNameOverride, true);
        },
        TracingUtils::SMITHY_CLIENT_SIGNING_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    if (!signedRequest)
    {
        AWS_LOGSTREAM_ERROR(AWS_CLIENT_LOG_TAG, "Request signing failed. Returning error.");
        return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::CLIENT_SIGNING_FAILURE, "", "SDK failed to sign the request", false/*retryable*/));
    }

    if (request.GetRequestSignedHandler())
    {
        request.GetRequestSignedHandler()(*httpRequest);
    }

    AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request Successfully signed");
    auto httpResponse = TracingUtils::MakeCallWithTiming<std::shared_ptr<HttpResponse>>(
        [&]() -> std::shared_ptr<HttpResponse> {
            return m_httpClient->MakeRequest(httpRequest, m_readRateLimiter.get(), m_writeRateLimiter.get());
        },
        TracingUtils::SMITHY_CLIENT_SERVICE_CALL_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, request.GetServiceRequestName()},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    context.SetTransmitResponse(httpResponse);
    for (const auto& interceptor : m_interceptors)
    {
        const auto modifiedRequest = interceptor->ModifyBeforeDeserialization(context);
        if (!modifiedRequest.IsSuccess())
        {
            return modifiedRequest.GetError();
        }
    }

    if (DoesResponseGenerateError(httpResponse) )
    {
        AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request returned error. Attempting to generate appropriate error codes from response");
        auto error = BuildAWSError(httpResponse);
        return HttpResponseOutcome(std::move(error));
    }
    else if(request.HasEmbeddedError(httpResponse->GetResponseBody(), httpResponse->GetHeaders()))
    {
        AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Response has embedded errors");

        auto error = GetErrorMarshaller()->Marshall(*httpResponse);
        return HttpResponseOutcome(std::move(error) );
    }

    AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request returned successful response.");

    return HttpResponseOutcome(std::move(httpResponse));
}

HttpResponseOutcome AWSClient::AttemptOneRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
    const char* signerName, const char* requestName, const char* signerRegionOverride, const char* signerServiceNameOverride) const
{
    AWS_UNREFERENCED_PARAM(requestName);

    auto signer = GetSignerByName(signerName);
    auto signedRequest = ::TracingUtils::MakeCallWithTiming<bool>([&]() -> bool {
            return signer->SignRequest(*httpRequest, signerRegionOverride, signerServiceNameOverride, true);
        },
        TracingUtils::SMITHY_CLIENT_SIGNING_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    if (!signedRequest)
    {
        AWS_LOGSTREAM_ERROR(AWS_CLIENT_LOG_TAG, "Request signing failed. Returning error.");
        return HttpResponseOutcome(AWSError<CoreErrors>(CoreErrors::CLIENT_SIGNING_FAILURE, "", "SDK failed to sign the request", false/*retryable*/));
    }

    //user agent and headers like that shouldn't be signed for the sake of compatibility with proxies which MAY mutate that header.
    AddCommonHeaders(*httpRequest);

    AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request Successfully signed");
    auto httpResponse = ::TracingUtils::MakeCallWithTiming<std::shared_ptr<HttpResponse>>(
        [&]() -> std::shared_ptr<HttpResponse> {
            return m_httpClient->MakeRequest(httpRequest, m_readRateLimiter.get(), m_writeRateLimiter.get());
        },
        TracingUtils::SMITHY_CLIENT_SERVICE_CALL_METRIC,
        *m_telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    if (DoesResponseGenerateError(httpResponse))
    {
        AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request returned error. Attempting to generate appropriate error codes from response");
        auto error = BuildAWSError(httpResponse);
        return HttpResponseOutcome(std::move(error));
    }

    AWS_LOGSTREAM_DEBUG(AWS_CLIENT_LOG_TAG, "Request returned successful response.");

    return HttpResponseOutcome(std::move(httpResponse));
}

StreamOutcome AWSClient::MakeRequestWithUnparsedResponse(const Aws::Http::URI& uri,
    const Aws::AmazonWebServiceRequest& request,
    Http::HttpMethod method,
    const char* signerName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpResponseOutcome = AttemptExhaustively(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride);
    if (httpResponseOutcome.IsSuccess())
    {
        return StreamOutcome(AmazonWebServiceResult<Stream::ResponseStream>(
            httpResponseOutcome.GetResult()->SwapResponseStreamOwnership(),
            httpResponseOutcome.GetResult()->GetHeaders(), httpResponseOutcome.GetResult()->GetResponseCode()));
    }

    return StreamOutcome(std::move(httpResponseOutcome));
}

StreamOutcome AWSClient::MakeRequestWithUnparsedResponse(const Aws::Http::URI& uri,
    Http::HttpMethod method,
    const char* signerName,
    const char* requestName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpResponseOutcome = AttemptExhaustively(uri, method, signerName, requestName, signerRegionOverride, signerServiceNameOverride);
    if (httpResponseOutcome.IsSuccess())
    {
        return StreamOutcome(AmazonWebServiceResult<Stream::ResponseStream>(
            httpResponseOutcome.GetResult()->SwapResponseStreamOwnership(),
            httpResponseOutcome.GetResult()->GetHeaders(), httpResponseOutcome.GetResult()->GetResponseCode()));
    }

    return StreamOutcome(std::move(httpResponseOutcome));
}

StreamOutcome AWSClient::MakeRequestWithUnparsedResponse(const Aws::AmazonWebServiceRequest& request,
                                                         const Aws::Endpoint::AWSEndpoint& endpoint,
                                                         Http::HttpMethod method,
                                                         const char* signerName,
                                                         const char* signerRegionOverride,
                                                         const char* signerServiceNameOverride) const
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

    return MakeRequestWithUnparsedResponse(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride);
}

XmlOutcome AWSXMLClient::MakeRequestWithEventStream(const Aws::AmazonWebServiceRequest& request,
                                                    const Aws::Endpoint::AWSEndpoint& endpoint,
                                                    Http::HttpMethod method,
                                                    const char* signerName,
                                                    const char* signerRegionOverride,
                                                    const char* signerServiceNameOverride) const
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

    return MakeRequestWithEventStream(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride);
}

XmlOutcome AWSXMLClient::MakeRequestWithEventStream(const Aws::Http::URI& uri,
    const Aws::AmazonWebServiceRequest& request,
    Http::HttpMethod method,
    const char* signerName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpOutcome = AttemptExhaustively(uri, request, method, signerName, signerRegionOverride, signerServiceNameOverride);
    if (httpOutcome.IsSuccess())
    {
        return XmlOutcome(AmazonWebServiceResult<XmlDocument>(XmlDocument(), httpOutcome.GetResult()->GetHeaders()));
    }

    return XmlOutcome(std::move(httpOutcome));
}

XmlOutcome AWSXMLClient::MakeRequestWithEventStream(const Aws::Http::URI& uri,
    Http::HttpMethod method,
    const char* signerName,
    const char* requestName,
    const char* signerRegionOverride,
    const char* signerServiceNameOverride) const
{
    HttpResponseOutcome httpOutcome = AttemptExhaustively(uri, method, signerName, requestName, signerRegionOverride, signerServiceNameOverride);
    if (httpOutcome.IsSuccess())
    {
        return XmlOutcome(AmazonWebServiceResult<XmlDocument>(XmlDocument(), httpOutcome.GetResult()->GetHeaders()));
    }

    return XmlOutcome(std::move(httpOutcome));
}

void AWSClient::AddHeadersToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
    const Http::HeaderValueCollection& headerValues) const
{
    for (auto const& headerValue : headerValues)
    {
        httpRequest->SetHeaderValue(headerValue.first, headerValue.second);
    }

    AddCommonHeaders(*httpRequest);
}

void AWSClient::AppendHeaderValueToRequest(const std::shared_ptr<Aws::Http::HttpRequest> &httpRequest, const String header, const String value) const
{
    if (!httpRequest->HasHeader(header.c_str()))
    {
        httpRequest->SetHeaderValue(header, value);
    }
    else
    {
        Aws::String contentEncoding = httpRequest->GetHeaderValue(header.c_str());
        contentEncoding.append(",").append(value);
        httpRequest->SetHeaderValue(header, contentEncoding);
    }
}

void AWSClient::AddContentBodyToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
    const std::shared_ptr<Aws::IOStream>& body, bool needsContentMd5, bool isChunked) const
{
    httpRequest->AddContentBody(body);

    //If there is no body, we have a content length of 0
    //note: we also used to remove content-type, but S3 actually needs content-type on InitiateMultipartUpload and it isn't
    //forbidden by the spec. If we start getting weird errors related to this, make sure it isn't caused by this removal.
    if (!body)
    {
        AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "No content body, content-length headers");

        if(httpRequest->GetMethod() == HttpMethod::HTTP_POST || httpRequest->GetMethod() == HttpMethod::HTTP_PUT)
        {
            httpRequest->SetHeaderValue(Http::CONTENT_LENGTH_HEADER, "0");
        }
        else
        {
            httpRequest->DeleteHeader(Http::CONTENT_LENGTH_HEADER);
        }
    }

    //Add transfer-encoding:chunked to header
    if (body && isChunked && !httpRequest->HasHeader(Http::CONTENT_LENGTH_HEADER))
    {
        httpRequest->SetTransferEncoding(CHUNKED_VALUE);
    }
    //in the scenario where we are adding a content body as a stream, the request object likely already
    //has a content-length header set and we don't want to seek the stream just to find this information.
    else if (body && !httpRequest->HasHeader(Http::CONTENT_LENGTH_HEADER))
    {
        if (!m_httpClient->SupportsChunkedTransferEncoding())
        {
            AWS_LOGSTREAM_WARN(AWS_CLIENT_LOG_TAG, "This http client doesn't support transfer-encoding:chunked. " <<
                                                   "The request may fail if it's not a seekable stream.");
        }
        AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Found body, but content-length has not been set, attempting to compute content-length");
        body->seekg(0, body->end);
        auto streamSize = body->tellg();
        body->seekg(0, body->beg);
        Aws::StringStream ss;
        ss << streamSize;
        httpRequest->SetContentLength(ss.str());
    }

    if (needsContentMd5 && body && !httpRequest->HasHeader(Http::CONTENT_MD5_HEADER))
    {
        AWS_LOGSTREAM_TRACE(AWS_CLIENT_LOG_TAG, "Found body, and content-md5 needs to be set" <<
            ", attempting to compute content-md5");

        //changing the internal state of the hash computation is not a logical state
        //change as far as constness goes for this class. Due to the platform specificness
        //of hash computations, we can't control the fact that computing a hash mutates
        //state on some platforms such as windows (but that isn't a concern of this class.
        auto md5HashResult = const_cast<AWSClient*>(this)->m_hash->Calculate(*body);
        body->clear();
        if (md5HashResult.IsSuccess())
        {
            httpRequest->SetHeaderValue(Http::CONTENT_MD5_HEADER, HashingUtils::Base64Encode(md5HashResult.GetResult()));
        }
    }
}

Aws::String Aws::Client::GetAuthorizationHeader(const Aws::Http::HttpRequest& httpRequest)
{
    // Extract the hex-encoded signature from the authorization header rather than recalculating it.
    assert(httpRequest.HasAwsAuthorization());
    const auto& authHeader = httpRequest.GetAwsAuthorization();
    auto signaturePosition = authHeader.rfind(Aws::Auth::SIGNATURE);
    // The auth header should end with 'Signature=<64 chars>'
    // Make sure we found the word 'Signature' in the header and make sure it's the last item followed by its 64 hex chars
    if (signaturePosition == Aws::String::npos || (signaturePosition + strlen(Aws::Auth::SIGNATURE) + 1/*'=' character*/ + 64/*hex chars*/) != authHeader.length())
    {
        AWS_LOGSTREAM_ERROR(AWS_CLIENT_LOG_TAG, "Failed to extract signature from authorization header.");
        return {};
    }
    return authHeader.substr(signaturePosition + strlen(Aws::Auth::SIGNATURE) + 1);
}

void AWSClient::BuildHttpRequest(const Aws::AmazonWebServiceRequest& request, const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest) const
{
    //do headers first since the request likely will set content-length as its own header.
    AddHeadersToRequest(httpRequest, request.GetHeaders());
    AddHeadersToRequest(httpRequest, request.GetAdditionalCustomHeaders());

    if (request.IsEventStreamRequest())
    {
        httpRequest->AddContentBody(request.GetBody());
    }
    else
    {
        //Check if compression is required
        CompressionAlgorithm selectedCompressionAlgorithm =
            request.GetSelectedCompressionAlgorithm(m_requestCompressionConfig);
        if (Aws::Client::CompressionAlgorithm::NONE != selectedCompressionAlgorithm) {
            Aws::Client::RequestCompression rc;
            auto compressOutcome = rc.compress(request.GetBody(), selectedCompressionAlgorithm);

            if (compressOutcome.IsSuccess()) {
                Aws::String compressionAlgorithmId = Aws::Client::GetCompressionAlgorithmId(selectedCompressionAlgorithm);
                AppendHeaderValueToRequest(httpRequest, CONTENT_ENCODING_HEADER, compressionAlgorithmId);
                AddContentBodyToRequest(
                    httpRequest, compressOutcome.GetResult(),
                    request.ShouldComputeContentMd5(),
                    request.IsStreaming() && request.IsChunked() &&
                        m_httpClient->SupportsChunkedTransferEncoding());
            } else {
                AWS_LOGSTREAM_ERROR(AWS_CLIENT_LOG_TAG, "Failed to compress request, submitting uncompressed");
                AddContentBodyToRequest(httpRequest, request.GetBody(), request.ShouldComputeContentMd5(), request.IsStreaming() && request.IsChunked() && m_httpClient->SupportsChunkedTransferEncoding());
            }
        } else {
            AddContentBodyToRequest(httpRequest, request.GetBody(), request.ShouldComputeContentMd5(), request.IsStreaming() && request.IsChunked() && m_httpClient->SupportsChunkedTransferEncoding());
        }
    }

    // Pass along handlers for processing data sent/received in bytes
    httpRequest->SetHeadersReceivedEventHandler(request.GetHeadersReceivedEventHandler());
    httpRequest->SetDataReceivedEventHandler(request.GetDataReceivedEventHandler());
    httpRequest->SetDataSentEventHandler(request.GetDataSentEventHandler());
    httpRequest->SetContinueRequestHandle(request.GetContinueRequestHandler());
    httpRequest->SetServiceSpecificParameters(request.GetServiceSpecificParameters());
    request.AddQueryStringParameters(httpRequest->GetUri());
}

void AWSClient::AddCommonHeaders(Aws::Http::HttpRequest& httpRequest) const
{
    httpRequest.SetUserAgent(m_userAgent);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, customizedHeaders, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, customizedHeaders, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, serviceName, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, serviceName, customizedHeaders, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const char* signerName, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, serviceName, signerName, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName, const char* signerName, const Aws::Http::HeaderValueCollection& customizedHeaders, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(uri, method, region, serviceName, signerName, customizedHeaders, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::Endpoint::AWSEndpoint& endpoint,
                                            Aws::Http::HttpMethod method /* = Http::HttpMethod::HTTP_POST */,
                                            const Aws::Http::HeaderValueCollection& customizedHeaders /* = {} */,
                                            uint64_t expirationInSeconds /* = 0 */,
                                            const char* signerName /* = Aws::Auth::SIGV4_SIGNER */,
                                            const char* signerRegionOverride /* = nullptr */,
                                            const char* signerServiceNameOverride /* = nullptr */,
                                            const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter)
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(endpoint, method, customizedHeaders, expirationInSeconds, signerName, signerRegionOverride, signerServiceNameOverride, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region,
    const Aws::Http::QueryStringParameterCollection& extraParams, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(request, uri, method, region, extraParams, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method, const char* region, const char* serviceName,
    const Aws::Http::QueryStringParameterCollection& extraParams, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(request, uri, method, region, serviceName, extraParams, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request,
                                            const Aws::Http::URI& uri,
                                            Aws::Http::HttpMethod method,
                                            const char* region,
                                            const char* serviceName,
                                            const char* signerName,
                                            const Aws::Http::QueryStringParameterCollection& extraParams,
                                            long long expirationInSeconds,
                                            const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(request, uri, method, region, serviceName, signerName, extraParams, expirationInSeconds, serviceSpecificParameter);
}

Aws::String AWSClient::GeneratePresignedUrl(const Aws::AmazonWebServiceRequest& request, const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
    const Aws::Http::QueryStringParameterCollection& extraParams, long long expirationInSeconds, const std::shared_ptr<Aws::Http::ServiceSpecificParameters> serviceSpecificParameter) const
{
    return AWSUrlPresigner(*this).GeneratePresignedUrl(request, uri, method, extraParams, expirationInSeconds, serviceSpecificParameter);
}

std::shared_ptr<Aws::Http::HttpResponse> AWSClient::MakeHttpRequest(std::shared_ptr<Aws::Http::HttpRequest>& request) const
{
    return m_httpClient->MakeRequest(request, m_readRateLimiter.get(), m_writeRateLimiter.get());
}

void AWSClient::AppendRecursionDetectionHeader(std::shared_ptr<Aws::Http::HttpRequest> ioRequest)
{
    if(!ioRequest || ioRequest->HasHeader(Aws::Http::X_AMZN_TRACE_ID_HEADER)) {
        return;
    }
    Aws::String awsLambdaFunctionName = Aws::Environment::GetEnv(AWS_LAMBDA_FUNCTION_NAME);
    if(awsLambdaFunctionName.empty()) {
        return;
    }
    Aws::String xAmznTraceIdVal = Aws::Environment::GetEnv(X_AMZN_TRACE_ID);
    if(xAmznTraceIdVal.empty()) {
        return;
    }

    // Escape all non-printable ASCII characters by percent encoding
    Aws::OStringStream xAmznTraceIdValEncodedStr;
    for(const char ch : xAmznTraceIdVal)
    {
        if (ch >= 0x20 && ch <= 0x7e) // ascii chars [32-126] or [' ' to '~'] are not escaped
        {
            xAmznTraceIdValEncodedStr << ch;
        }
        else
        {
            // A percent-encoded octet is encoded as a character triplet
            xAmznTraceIdValEncodedStr << '%' // consisting of the percent character "%"
                                      << std::hex << std::setfill('0') << std::setw(2) << std::uppercase
                                      << (size_t) ch //followed by the two hexadecimal digits representing that octet's numeric value
                                      << std::dec << std::setfill(' ') << std::setw(0) << std::nouppercase;
        }
    }
    xAmznTraceIdVal = xAmznTraceIdValEncodedStr.str();

    ioRequest->SetHeaderValue(Aws::Http::X_AMZN_TRACE_ID_HEADER, xAmznTraceIdVal);
}