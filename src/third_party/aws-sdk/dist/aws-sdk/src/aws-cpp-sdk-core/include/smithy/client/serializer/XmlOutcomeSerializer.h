/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <aws/core/AmazonWebServiceResult.h>
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/Outcome.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <smithy/tracing/TracingUtils.h>
#include <smithy/tracing/TelemetryProvider.h>

namespace smithy
{
    namespace client
    {
        using TracingUtils = components::tracing::TracingUtils;
        using CoreErrors = Aws::Client::CoreErrors;
        using AWSError = Aws::Client::AWSError<CoreErrors>;
        using XmlDocument = Aws::Utils::Xml::XmlDocument;
        using HttpResponseOutcome = Aws::Utils::Outcome<std::shared_ptr<Aws::Http::HttpResponse>, AWSError>;
        using XmlServiceResult = Aws::AmazonWebServiceResult<XmlDocument>;
        using XmlOutcome = Aws::Utils::Outcome<XmlServiceResult, AWSError>;
        using TelemetryProvider = components::tracing::TelemetryProvider;

        class XmlOutcomeSerializer
        {
        public:
            explicit XmlOutcomeSerializer(const std::shared_ptr<TelemetryProvider>& telemetryProvider)
                : m_telemetryProvider(telemetryProvider)
            {
            }

            XmlOutcomeSerializer(const XmlOutcomeSerializer& other) = delete;
            XmlOutcomeSerializer(XmlOutcomeSerializer&& other) noexcept = default;
            XmlOutcomeSerializer& operator=(const XmlOutcomeSerializer& other) = delete;
            XmlOutcomeSerializer& operator=(XmlOutcomeSerializer&& other) noexcept = default;
            virtual ~XmlOutcomeSerializer() = default;


            XmlOutcome Deserialize(HttpResponseOutcome&& httpOutcome,
                                   const Aws::String& serviceName,
                                   const Aws::String& requestName) const
            {
                if (!httpOutcome.IsSuccess())
                {
                    return TracingUtils::MakeCallWithTiming<XmlOutcome>(
                        [&]() -> XmlOutcome {
                            return {std::move(httpOutcome)};
                        },
                        TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                        *m_telemetryProvider->getMeter(serviceName, {}),
                        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName}, {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
                }

                if (httpOutcome.GetResult()->GetResponseBody().good() &&
                    httpOutcome.GetResult()->GetResponseBody().tellp() > 0)
                {
                    return TracingUtils::MakeCallWithTiming<XmlOutcome>(
                        [&]() -> XmlOutcome {
                            XmlDocument xmlDoc = XmlDocument::CreateFromXmlStream(httpOutcome.GetResult()->GetResponseBody());

                            if (!xmlDoc.WasParseSuccessful())
                            {
                                AWS_LOGSTREAM_ERROR("XmlOutcomeSerializer", "Xml parsing for error failed with message " << xmlDoc.GetErrorMessage().c_str());
                                return AWSError(CoreErrors::UNKNOWN,
                                    "Xml Parse Error",
                                    xmlDoc.GetErrorMessage(),
                                    false);
                            }

                            return {XmlServiceResult(std::move(xmlDoc),
                                httpOutcome.GetResult()->GetHeaders(),
                                httpOutcome.GetResult()->GetResponseCode())};
                        },
                        TracingUtils::SMITHY_CLIENT_DESERIALIZATION_METRIC,
                        *m_telemetryProvider->getMeter(serviceName, {}),
                        {{TracingUtils::SMITHY_METHOD_DIMENSION, requestName}, {TracingUtils::SMITHY_SERVICE_DIMENSION, serviceName}});
                }

                return {XmlServiceResult(XmlDocument(), httpOutcome.GetResult()->GetHeaders())};
            }
        private:
            std::shared_ptr<TelemetryProvider> m_telemetryProvider;
        };

    } // namespace client
} // namespace smithy
