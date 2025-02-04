/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <smithy/client/AwsSmithyClientBase.h>
#include <smithy/client/AwsSmithyClientAsyncRequestContext.h>
#include <smithy/client/features/RecursionDetection.h>
#include <smithy/client/features/RequestPayloadCompression.h>

#include "aws/core/client/AWSErrorMarshaller.h"
#include "aws/core/client/RetryStrategy.h"
#include "aws/core/http/HttpClientFactory.h"
#include "aws/core/monitoring/CoreMetrics.h"
#include "aws/core/monitoring/MonitoringManager.h"
#include "aws/core/utils/DNS.h"
#include "aws/core/utils/threading/Executor.h"
#include "aws/core/utils/threading/SameThreadExecutor.h"
#include "smithy/tracing/TracingUtils.h"

using namespace smithy::client;
using namespace smithy::interceptor;
using namespace smithy::components::tracing;

static const char AWS_SMITHY_CLIENT_LOG[] = "AwsSmithyClient";


void AddHeadersToRequest(const std::shared_ptr<Aws::Http::HttpRequest>& httpRequest,
    const Aws::Http::HeaderValueCollection& headerValues)
{
    for (auto const& headerValue : headerValues)
    {
        httpRequest->SetHeaderValue(headerValue.first, headerValue.second);
    }
}

std::shared_ptr<Aws::Http::HttpRequest>
AwsSmithyClientBase::BuildHttpRequest(const std::shared_ptr<AwsSmithyClientAsyncRequestContext>& pRequestCtx,
                                      const Aws::Http::URI& uri,
                                      Aws::Http::HttpMethod method
                                      ) const
{
    assert(pRequestCtx);
    std::shared_ptr<HttpRequest> httpRequest;
    const auto* pRequest = pRequestCtx->m_pRequest;
    if(pRequest) {
        httpRequest = CreateHttpRequest(uri, method, pRequest->GetResponseStreamFactory());
    } else {
        httpRequest = CreateHttpRequest(uri, method, Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
    }
    if (!httpRequest)
    {
        AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_LOG, "Failed to allocate an HttpRequest under a shared ptr");
        return httpRequest;
    }

    httpRequest->SetUserAgent(m_userAgent);
    httpRequest->SetHeaderValue(Aws::Http::SDK_INVOCATION_ID_HEADER, pRequestCtx->m_invocationId);
    httpRequest->SetHeaderValue(Aws::Http::SDK_REQUEST_HEADER, pRequestCtx->m_requestInfo.ToString());
    RecursionDetection::AppendRecursionDetectionHeader(pRequestCtx->m_httpRequest);

    if (pRequest)
    {
        AddHeadersToRequest(httpRequest, pRequest->GetHeaders());
        AddHeadersToRequest(httpRequest, pRequest->GetAdditionalCustomHeaders());

        if (pRequest->IsEventStreamRequest())
        {
            httpRequest->SetEventStreamRequest(true);
            httpRequest->AddContentBody(pRequest->GetBody());
        }
        else
        {
            //Check if compression is required
            Aws::Client::CompressionAlgorithm selectedCompressionAlgorithm = pRequest->GetSelectedCompressionAlgorithm(m_clientConfig->requestCompressionConfig);
            if (Aws::Client::CompressionAlgorithm::NONE != selectedCompressionAlgorithm) {
                RequestPayloadCompression::AddCompressedContentBodyToRequest(pRequest, httpRequest, selectedCompressionAlgorithm, m_httpClient);
            } else {
                Utils::AddContentBodyToRequest(httpRequest, pRequest->GetBody(), m_httpClient,
                                               pRequest->ShouldComputeContentMd5(), pRequest->IsStreaming() && pRequest->IsChunked());
            }
        }

        // Pass along handlers for processing data sent/received in bytes
        httpRequest->SetHeadersReceivedEventHandler(pRequest->GetHeadersReceivedEventHandler());
        httpRequest->SetDataReceivedEventHandler(pRequest->GetDataReceivedEventHandler());
        httpRequest->SetDataSentEventHandler(pRequest->GetDataSentEventHandler());
        httpRequest->SetContinueRequestHandle(pRequest->GetContinueRequestHandler());
        httpRequest->SetServiceSpecificParameters(pRequest->GetServiceSpecificParameters());

        pRequest->AddQueryStringParameters(httpRequest->GetUri());
    }

    return httpRequest;
}

void AwsSmithyClientBase::MakeRequestAsync(Aws::AmazonWebServiceRequest const* const request,
                                           const char* requestName,
                                           Aws::Http::HttpMethod method,
                                           EndpointUpdateCallback&& endpointCallback,
                                           ResponseHandlerFunc&& responseHandler,
                                           std::shared_ptr<Aws::Utils::Threading::Executor> pExecutor) const
{
    if(!responseHandler)
    {
        assert(!"Missing a mandatory response handler!");
        AWS_LOGSTREAM_FATAL(AWS_SMITHY_CLIENT_LOG, "Unable to continue AWSClient request: response handler is missing!");
        return;
    }

    std::shared_ptr<AwsSmithyClientAsyncRequestContext> pRequestCtx =
        Aws::MakeShared<AwsSmithyClientAsyncRequestContext>(AWS_SMITHY_CLIENT_LOG);
    if (!pRequestCtx)
    {
        AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_LOG, "Failed to allocate an AwsSmithyClientAsyncRequestContext under a shared ptr");
        auto outcome = HttpResponseOutcome(ClientError(CoreErrors::MEMORY_ALLOCATION, "", "Failed to allocate async request context", false/*retryable*/));
        pExecutor->Submit([outcome, responseHandler]() mutable
          {
              responseHandler(std::move(outcome));
          } );
        return;
    }
    pRequestCtx->m_responseHandler = std::move(responseHandler);
    pRequestCtx->m_pExecutor = pExecutor;
    pRequestCtx->m_pRequest = request;
    if (requestName)
      pRequestCtx->m_requestName = requestName;
    else if (pRequestCtx->m_pRequest)
      pRequestCtx->m_requestName = pRequestCtx->m_pRequest->GetServiceRequestName();
    pRequestCtx->m_method = method;
    pRequestCtx->m_retryCount = 0;
    pRequestCtx->m_invocationId = Aws::Utils::UUID::PseudoRandomUUID();
    auto authSchemeOptionOutcome = this->SelectAuthSchemeOption(*pRequestCtx);
    if (!authSchemeOptionOutcome.IsSuccess())
    {
        pExecutor->Submit([authSchemeOptionOutcome, responseHandler]() mutable
          {
              responseHandler(std::move(authSchemeOptionOutcome));
          } );
        return;
    }
    pRequestCtx->m_authSchemeOption = std::move(authSchemeOptionOutcome.GetResultWithOwnership());
    assert(pRequestCtx->m_authSchemeOption.schemeId);
    Aws::Endpoint::EndpointParameters epParams = request ? request->GetEndpointContextParams() : Aws::Endpoint::EndpointParameters();
    const auto authSchemeEpParams = pRequestCtx->m_authSchemeOption.endpointParameters();
    epParams.insert(epParams.end(), authSchemeEpParams.begin(), authSchemeEpParams.end());
    auto epResolutionOutcome = this->ResolveEndpoint(std::move(epParams), std::move(endpointCallback));
    if (!epResolutionOutcome.IsSuccess())
    {
        pExecutor->Submit([epResolutionOutcome, responseHandler]() mutable
          {
              responseHandler(std::move(epResolutionOutcome));
          } );
        return;
    }
    pRequestCtx->m_endpoint = std::move(epResolutionOutcome.GetResultWithOwnership());
    if (!Aws::Utils::IsValidHost(pRequestCtx->m_endpoint.GetURI().GetAuthority()))
    {
        AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_LOG, "Invalid DNS Label found in URI host");
        auto outcome = HttpResponseOutcome(ClientError(CoreErrors::VALIDATION, "", "Invalid DNS Label found in URI host", false/*retryable*/));
        pExecutor->Submit([outcome, responseHandler]() mutable
          {
              responseHandler(std::move(outcome));
          } );
        return;
    }
    pRequestCtx->m_requestInfo.attempt = 1;
    pRequestCtx->m_requestInfo.maxAttempts = 0;
    pRequestCtx->m_interceptorContext = Aws::MakeShared<InterceptorContext>(AWS_SMITHY_CLIENT_LOG, *request);

    AttemptOneRequestAsync(std::move(pRequestCtx));
}

/*HttpResponseOutcome*/
void AwsSmithyClientBase::AttemptOneRequestAsync(std::shared_ptr<AwsSmithyClientAsyncRequestContext> pRequestCtx) const
{
    if(!pRequestCtx)
    {
        assert(!"Missing pRequestCtx");
        AWS_LOGSTREAM_FATAL(AWS_SMITHY_CLIENT_LOG, "Missing request context!");
    }
    auto& responseHandler = pRequestCtx->m_responseHandler;
    auto pExecutor = pRequestCtx->m_pExecutor;

    TracingUtils::MakeCallWithTiming(
      [&]() -> void {
          pRequestCtx->m_httpRequest = BuildHttpRequest(pRequestCtx, pRequestCtx->m_endpoint.GetURI(), pRequestCtx->m_method);
      },
      TracingUtils::SMITHY_CLIENT_SERIALIZATION_METRIC,
      *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
      {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
       {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    if (!pRequestCtx->m_httpRequest)
    {
        AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_LOG, "Failed to BuildHttpRequest");
        auto outcome = HttpResponseOutcome(ClientError(CoreErrors::VALIDATION, "", "Unable to create HttpRequest object", false/*retryable*/));
        pExecutor->Submit([outcome, responseHandler]() mutable
          {
              responseHandler(std::move(outcome));
          } );
        return;
    }

    pRequestCtx->m_interceptorContext->SetTransmitRequest(pRequestCtx->m_httpRequest);
    for (const auto& interceptor : m_interceptors)
    {
        auto modifiedRequest = interceptor->ModifyBeforeSigning(*pRequestCtx->m_interceptorContext);
        if (!modifiedRequest.IsSuccess())
        {
            pExecutor->Submit([modifiedRequest, responseHandler]() mutable
              {
                  responseHandler(modifiedRequest.GetError());
              });
            return;
        }
    }

    Aws::Monitoring::CoreMetricsCollection coreMetrics;
    pRequestCtx->m_monitoringContexts = Aws::Monitoring::OnRequestStarted(this->GetServiceClientName(),
                                                                          pRequestCtx->m_requestName,
                                                                          pRequestCtx->m_httpRequest);

    if(m_clientConfig->retryStrategy && !m_clientConfig->retryStrategy->HasSendToken())
    {
        auto errOutcome = HttpResponseOutcome(ClientError(CoreErrors::SLOW_DOWN,
                                                                "",
                                                                "Unable to acquire enough send tokens to execute request.",
                                                                false/*retryable*/));
        pExecutor->Submit([errOutcome, responseHandler]() mutable
          {
              responseHandler(std::move(errOutcome));
          } );
        return;
    };

    SigningOutcome signingOutcome = TracingUtils::MakeCallWithTiming<SigningOutcome>([&]() -> SigningOutcome {
            return this->SignRequest(pRequestCtx->m_httpRequest, pRequestCtx->m_authSchemeOption);
        },
        TracingUtils::SMITHY_CLIENT_SIGNING_METRIC,
        *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
         {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    if (!signingOutcome.IsSuccess())
    {
        AWS_LOGSTREAM_ERROR(AWS_SMITHY_CLIENT_LOG, "Request signing failed. Returning error.");
        auto errOutcome = HttpResponseOutcome(ClientError(CoreErrors::CLIENT_SIGNING_FAILURE, "", "SDK failed to sign the request", false/*retryable*/));
        pRequestCtx->m_pExecutor->Submit([errOutcome, pRequestCtx]() mutable
          {
              pRequestCtx->m_responseHandler(std::move(errOutcome));
          } );
        return;
    }

    std::shared_ptr<Aws::Http::HttpRequest> signedHttpRequest = signingOutcome.GetResultWithOwnership();
    assert(signedHttpRequest);

    if (pRequestCtx->m_pRequest && pRequestCtx->m_pRequest->GetRequestSignedHandler())
    {
        pRequestCtx->m_pRequest->GetRequestSignedHandler()(*signedHttpRequest);
    }

    // handler for a single http reply (vs final AWS response handler)
    auto httpResponseHandler = [this, pRequestCtx](std::shared_ptr<Aws::Http::HttpResponse> pResponse) mutable
    {
        HandleAsyncReply(std::move(pRequestCtx), std::move(pResponse));
    };

    // TODO: async http client
#if 0
    AWS_LOGSTREAM_DEBUG(AWS_SMITHY_CLIENT_LOG, "Request Successfully signed");
    TracingUtils::MakeCallWithTiming(
        [&]() -> void {
            m_httpClient->MakeAsyncRequest(pRequestCtx->m_httpRequest,
                                           pRequestCtx->m_pExecutor,
                                           responseHandler,
                                           m_clientConfig->readRateLimiter.get(),
                                           m_clientConfig->writeRateLimiter.get());
    },
    TracingUtils::SMITHY_CLIENT_SERVICE_CALL_METRIC,
    *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
    {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
     {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
#else
    auto httpResponse = TracingUtils::MakeCallWithTiming<std::shared_ptr<HttpResponse>>(
        [&]() -> std::shared_ptr<HttpResponse> {
            return m_httpClient->MakeRequest(signedHttpRequest, m_clientConfig->readRateLimiter.get(), m_clientConfig->writeRateLimiter.get());
        },
        TracingUtils::SMITHY_CLIENT_SERVICE_CALL_METRIC,
        *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
        {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},{TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});

    pRequestCtx->m_pExecutor->Submit([httpResponse, httpResponseHandler]() mutable
      {
          httpResponseHandler(std::move(httpResponse));
      } );
#endif
    return;
}

void AwsSmithyClientBase::HandleAsyncReply(std::shared_ptr<AwsSmithyClientAsyncRequestContext> pRequestCtx,
                                           std::shared_ptr<Aws::Http::HttpResponse> httpResponse) const
{
    assert(pRequestCtx && httpResponse);

    pRequestCtx->m_interceptorContext->SetTransmitResponse(httpResponse);
    for (const auto& interceptor : m_interceptors)
    {
        const auto modifiedResponse = interceptor->ModifyBeforeDeserialization(*pRequestCtx->m_interceptorContext);
        if (!modifiedResponse.IsSuccess())
        {
            return pRequestCtx->m_responseHandler(HttpResponseOutcome(modifiedResponse.GetError()));
        }
    };

    Aws::Client::HttpResponseOutcome outcome = [&]()
    {
        bool hasEmbeddedError = pRequestCtx->m_pRequest &&
                      pRequestCtx->m_pRequest->HasEmbeddedError(httpResponse->GetResponseBody(), httpResponse->GetHeaders());
        if (Utils::DoesResponseGenerateError(httpResponse) || hasEmbeddedError)
        {
            AWS_LOGSTREAM_DEBUG(AWS_SMITHY_CLIENT_LOG, "Request returned error. Attempting to generate appropriate error codes from response");
            assert(m_errorMarshaller);
            auto error = m_errorMarshaller->BuildAWSError(httpResponse);
            return HttpResponseOutcome(std::move(error));
        }
        AWS_LOGSTREAM_DEBUG(AWS_SMITHY_CLIENT_LOG, "Request returned successful response.");
        return HttpResponseOutcome(std::move(httpResponse));
    } ();

    Aws::Monitoring::CoreMetricsCollection coreMetrics;

    do // goto in a form of "do { break; } while(0);" // TODO: refactor
    {
        if (pRequestCtx->m_retryCount == 0)
        {
            m_clientConfig->retryStrategy->RequestBookkeeping(outcome);
        }
        else
        {
            assert(pRequestCtx->m_lastError);
            m_clientConfig->retryStrategy->RequestBookkeeping(outcome, pRequestCtx->m_lastError.value());
        }
        coreMetrics.httpClientMetrics = pRequestCtx->m_httpRequest->GetRequestMetrics();
        TracingUtils::EmitCoreHttpMetrics(pRequestCtx->m_httpRequest->GetRequestMetrics(),
            *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
             {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        if (outcome.IsSuccess())
        {
            Aws::Monitoring::OnRequestSucceeded(this->GetServiceClientName(),
                                                pRequestCtx->m_requestName,
                                                pRequestCtx->m_httpRequest,
                                                outcome,
                                                coreMetrics,
                                                pRequestCtx->m_monitoringContexts);
            AWS_LOGSTREAM_TRACE(AWS_SMITHY_CLIENT_LOG, "Request successful returning.");
            break;
        }
        pRequestCtx->m_lastError = outcome.GetError();

        Utils::DateTime serverTime = Utils::GetServerTimeFromError(outcome.GetError());
        auto clockSkew = Utils::DateTime::Diff(serverTime, Utils::DateTime::Now());

        Aws::Monitoring::OnRequestFailed(this->GetServiceClientName(),
                                         pRequestCtx->m_requestName,
                                         pRequestCtx->m_httpRequest,
                                         outcome,
                                         coreMetrics,
                                         pRequestCtx->m_monitoringContexts);

        if (!m_httpClient->IsRequestProcessingEnabled())
        {
            AWS_LOGSTREAM_TRACE(AWS_SMITHY_CLIENT_LOG, "Request was cancelled externally.");
            break;
        }

        // Adjust region
        // TODO: extract into common func
        bool retryWithCorrectRegion = [&]() {
            using HttpResponseCode = Aws::Http::HttpResponseCode;
            const HttpResponseCode httpResponseCode = outcome.GetError().GetResponseCode();
            if (httpResponseCode == HttpResponseCode::MOVED_PERMANENTLY ||  // 301
                httpResponseCode == HttpResponseCode::TEMPORARY_REDIRECT || // 307
                httpResponseCode == HttpResponseCode::BAD_REQUEST ||        // 400
                httpResponseCode == HttpResponseCode::FORBIDDEN)            // 403
            {
              assert(m_errorMarshaller);
              const Aws::String regionFromResponse = m_errorMarshaller->ExtractRegion(outcome.GetError());
              const Aws::String signerRegion = [&]() {
                  if (!regionFromResponse.empty() &&
                      pRequestCtx->m_endpoint.GetAttributes() &&
                      pRequestCtx->m_endpoint.GetAttributes()->authScheme.GetSigningRegion())
                  {
                      return pRequestCtx->m_endpoint.GetAttributes()->authScheme.GetSigningRegion().value();
                  }
                  return Aws::String("");
              } ();
              if (m_clientConfig->region == Aws::Region::AWS_GLOBAL && !regionFromResponse.empty() &&
                  regionFromResponse != signerRegion) {
                pRequestCtx->m_endpoint.AccessAttributes()->authScheme.SetSigningRegion(regionFromResponse);
                AWS_LOGSTREAM_DEBUG(AWS_SMITHY_CLIENT_LOG, "Need to retry with a correct region");
                return true;
              }
            }
            return false;
        } (); // <- immediately invoked lambda

        long sleepMillis = TracingUtils::MakeCallWithTiming<long>(
            [&]() -> long {
                return m_clientConfig->retryStrategy->CalculateDelayBeforeNextRetry(outcome.GetError(), static_cast<long>(pRequestCtx->m_retryCount));
            },
            TracingUtils::SMITHY_CLIENT_SERVICE_BACKOFF_DELAY_METRIC,
            *m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {}),
            {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
             {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
        bool shouldSleep = !retryWithCorrectRegion;
        if (m_clientConfig->enableClockSkewAdjustment)
        {
            // AdjustClockSkew returns true means clock skew was the problem and skew was adjusted, false otherwise.
            // sleep if clock skew and region was NOT the problem. AdjustClockSkew may update error inside outcome.
            shouldSleep |= !this->AdjustClockSkew(outcome, pRequestCtx->m_authSchemeOption);
        }

        if (!retryWithCorrectRegion && !m_clientConfig->retryStrategy->ShouldRetry(outcome.GetError(), static_cast<long>(pRequestCtx->m_retryCount)))
        {
            break;
        }

        AWS_LOGSTREAM_WARN(AWS_SMITHY_CLIENT_LOG, "Request failed, now waiting " << sleepMillis << " ms before attempting again.");

        if(pRequestCtx->m_pRequest) {
          if (pRequestCtx->m_pRequest->GetBody()) {
            pRequestCtx->m_pRequest->GetBody()->clear();
            pRequestCtx->m_pRequest->GetBody()->seekg(0);
          }

          if (pRequestCtx->m_pRequest->GetRequestRetryHandler()) {
            pRequestCtx->m_pRequest->GetRequestRetryHandler()(*pRequestCtx->m_pRequest);
          }
        }

        if (shouldSleep)
        {
            m_httpClient->RetryRequestSleep(std::chrono::milliseconds(sleepMillis));
        }

        if (retryWithCorrectRegion)
        {
            Aws::String newEndpoint = m_errorMarshaller->ExtractEndpoint(outcome.GetError());
            if (newEndpoint.empty()) {
              Aws::Http::URI newUri = pRequestCtx->m_endpoint.GetURI();
              newUri.SetAuthority(newEndpoint);
              pRequestCtx->m_endpoint.SetURI(newUri);
              AWS_LOGSTREAM_DEBUG(AWS_SMITHY_CLIENT_LOG, "Endpoint has been updated to " << newUri.GetURIString());
            }
        }
        if (serverTime.WasParseSuccessful() && serverTime != Utils::DateTime())
        {
            pRequestCtx->m_requestInfo.ttl = Utils::DateTime::Now() + clockSkew + std::chrono::milliseconds(m_clientConfig->requestTimeoutMs);
        }
        pRequestCtx->m_requestInfo.attempt ++;
        pRequestCtx->m_requestInfo.maxAttempts = m_clientConfig->retryStrategy->GetMaxAttempts();

        Aws::Monitoring::OnRequestRetry(this->GetServiceClientName(), pRequestCtx->m_requestName, pRequestCtx->m_httpRequest, pRequestCtx->m_monitoringContexts);

        pRequestCtx->m_retryCount++;
        AttemptOneRequestAsync(std::move(pRequestCtx));
        return;
    } while(false); // end of goto in a form of "do { break; } while(false);"

    auto meter = m_clientConfig->telemetryProvider->getMeter(this->GetServiceClientName(), {});
    auto counter = meter->CreateCounter(TracingUtils::SMITHY_CLIENT_SERVICE_ATTEMPTS_METRIC, TracingUtils::COUNT_METRIC_TYPE, "");
    counter->add(pRequestCtx->m_requestInfo.attempt, {{TracingUtils::SMITHY_METHOD_DIMENSION, pRequestCtx->m_requestName},
                                                      {TracingUtils::SMITHY_SERVICE_DIMENSION, this->GetServiceClientName()}});
    Aws::Monitoring::OnFinish(this->GetServiceClientName(), pRequestCtx->m_requestName, pRequestCtx->m_httpRequest, pRequestCtx->m_monitoringContexts);

    return pRequestCtx->m_responseHandler(std::move(outcome));
}

AwsSmithyClientBase::HttpResponseOutcome
AwsSmithyClientBase::MakeRequestSync(Aws::AmazonWebServiceRequest const * const request,
                                     const char* requestName,
                                     Aws::Http::HttpMethod method,
                                     EndpointUpdateCallback&& endpointCallback) const
{
    std::shared_ptr<Aws::Utils::Threading::Executor> pExecutor = Aws::MakeShared<Aws::Utils::Threading::SameThreadExecutor>(AWS_SMITHY_CLIENT_LOG);
    assert(pExecutor);

    HttpResponseOutcome outcome = ClientError(CoreErrors::INTERNAL_FAILURE, "", "Response handler was not called", false);
    ResponseHandlerFunc responseHandler = [&outcome](HttpResponseOutcome&& asyncOutcome)
    {
        outcome = std::move(asyncOutcome);
    };

    pExecutor->Submit([&]()
    {
        this->MakeRequestAsync(request, requestName, method, std::move(endpointCallback), std::move(responseHandler), pExecutor);
    });
    pExecutor->WaitUntilStopped();

    return outcome;
}

void AwsSmithyClientBase::DisableRequestProcessing()
{
    m_httpClient->DisableRequestProcessing();
}
