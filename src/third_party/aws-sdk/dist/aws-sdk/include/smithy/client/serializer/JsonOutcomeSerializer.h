/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <smithy/tracing/TracingUtils.h>
#include <smithy/tracing/TelemetryProvider.h>

namespace smithy
{
    namespace client
    {
        using TracingUtils = components::tracing::TracingUtils;
        using CoreErrors = Aws::Client::CoreErrors;
        using AWSError = Aws::Client::AWSError<CoreErrors>;
        using JsonValue = Aws::Utils::Json::JsonValue;
        using HttpResponseOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AWSError>;
        using JsonOutcome = Aws::Utils::Outcome<Aws::AmazonWebServiceResult<JsonValue>, AWSError>;
        using TelemetryProvider = components::tracing::TelemetryProvider;

        class JsonOutcomeSerializer
        {
        public:
            explicit JsonOutcomeSerializer(const std::shared_ptr<TelemetryProvider>& telemetryProvider)
                : m_telemetryProvider(telemetryProvider)
            {
            }

            JsonOutcomeSerializer(const JsonOutcomeSerializer& other) = delete;
            JsonOutcomeSerializer(JsonOutcomeSerializer&& other) noexcept = default;
            JsonOutcomeSerializer& operator=(const JsonOutcomeSerializer& other) = delete;
            JsonOutcomeSerializer& operator=(JsonOutcomeSerializer&& other) noexcept = default;
            virtual ~JsonOutcomeSerializer() = default;

            JsonOutcome Deserialize(HttpResponseOutcome&& httpOutcome,
                                    const Aws::String& serviceName,
                                    const Aws::String& requestName) const
            {
                if (!httpOutcome.IsSuccess())
                {
                    return TracingUtils::MakeCallWithTiming<JsonOutcome>(
                        [&]() -> JsonOutcome {
                            return JsonOutcome{std::move(httpOutcome)};
                        },
                        TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                        *m_telemetryProvider->getMeter(serviceName, {}),
                        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},
                         {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
                }

                if (httpOutcome.GetResult()->GetResponseBody().good() &&
                    httpOutcome.GetResult()->GetResponseBody().tellp() > 0)
                {
                    JsonValue jsonValue(httpOutcome.GetResult()->GetResponseBody());
                    if (!jsonValue.WasParseSuccessful()) {
                        return TracingUtils::MakeCallWithTiming<JsonOutcome>(
                            [&]() -> JsonOutcome {
                                return JsonOutcome{AWSError(CoreErrors::UNKNOWN,
                                    "Json Parser Error",
                                    jsonValue.GetErrorMessage(),
                                    false)};
                            },
                            TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                            *m_telemetryProvider->getMeter(serviceName, {}),
                            {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},
                             {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
                    }

                    return TracingUtils::MakeCallWithTiming<JsonOutcome>(
                        [&]() -> JsonOutcome {
                            return JsonOutcome{Aws::AmazonWebServiceResult<JsonValue>(std::move(jsonValue),
                                httpOutcome.GetResult()->GetHeaders(),
                                httpOutcome.GetResult()->GetResponseCode())};
                        },
                        TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                        *m_telemetryProvider->getMeter(serviceName, {}),
                        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},
                         {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
                }

                return TracingUtils::MakeCallWithTiming<JsonOutcome>(
                    [&]() -> JsonOutcome {
                        return JsonOutcome{Aws::AmazonWebServiceResult<JsonValue>(JsonValue(),
                            httpOutcome.GetResult()->GetHeaders())};
                    },
                    TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                    *m_telemetryProvider->getMeter(serviceName, {}),
                    {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName},
                     {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
            }

        private:
            std::shared_ptr<TelemetryProvider> m_telemetryProvider;
        };
    } // namespace client
} // namespace smithy
