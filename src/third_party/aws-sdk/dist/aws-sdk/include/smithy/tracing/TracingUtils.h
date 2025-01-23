/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/monitoring/HttpClientMetrics.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <smithy/Smithy_EXPORTS.h>
#include <smithy/tracing/Meter.h>
#include <functional>
#include <chrono>
#include <utility>

namespace smithy {
    namespace components {
        namespace tracing {
            /**
             * A utility class for common tracing activities.
             */
            class SMITHY_API TracingUtils {
            public:
                TracingUtils() = default;

                static const char COUNT_METRIC_TYPE[];
                static const char MICROSECOND_METRIC_TYPE[];
                static const char BYTES_PER_SECOND_METRIC_TYPE[];
                static const char SMITHY_CLIENT_DURATION_METRIC[];
                static const char SMITHY_CLIENT_ENDPOINT_RESOLUTION_METRIC[];
                static const char SMITHY_CLIENT_DESERIALIZATION_METRIC[];
                static const char SMITHY_CLIENT_SIGNING_METRIC[];
                static const char SMITHY_CLIENT_SERIALIZATION_METRIC[];
                static const char SMITHY_CLIENT_SERVICE_CALL_METRIC[];
                static const char SMITHY_CLIENT_SERVICE_BACKOFF_DELAY_METRIC[];
                static const char SMITHY_CLIENT_SERVICE_ATTEMPTS_METRIC[];
                static const char SMITHY_METHOD_AWS_VALUE[];
                static const char SMITHY_SERVICE_DIMENSION[];
                static const char SMITHY_METHOD_DIMENSION[];
                static const char SMITHY_SYSTEM_DIMENSION[];
                static const char SMITHY_METRICS_DNS_DURATION[];
                static const char SMITHY_METRICS_CONNECT_DURATION[];
                static const char SMITHY_METRICS_SSL_DURATION[];
                static const char SMITHY_METRICS_DOWNLOAD_SPEED_METRIC[];
                static const char SMITHY_METRICS_UPLOAD_SPEED_METRIC[];
                static const char SMITHY_METRICS_UNKNOWN_METRIC[];

                /**
                 * Will run a function and emit the duration of that function in millisecond timing to the
                 * meter provided as a Histogram metrics. Will return the result af the function.
                 * @tparam T The type that is being returned from the function.
                 * @param func A function that returns T.
                 * @param metricName The name of the metric that is being captured by the function.
                 * @param meter The meter making the measurement.
                 * @param attributes The attributes or dimensions associate with this measurement.
                 * @param description The description of the measurement.
                 * @return the result of func.
                 */
                template<typename T>
                static T MakeCallWithTiming(std::function<T()> func,
                    const Aws::String &metricName,
                    const Meter &meter,
                    Aws::Map<Aws::String, Aws::String>&& attributes,
                    const Aws::String &description = "")
                {
                    auto before = std::chrono::steady_clock::now();
                    auto returnValue = func();
                    auto after = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(after - before).count();
                    auto histogram = meter.CreateHistogram(metricName, MICROSECOND_METRIC_TYPE, description);
                    if (!histogram) {
                        AWS_LOG_ERROR("TracingUtil", "Failed to create histogram");
                        return {};
                    }
                    histogram->record((double) duration, std::forward<Aws::Map<Aws::String, Aws::String>>(attributes));
                    return returnValue;
                }

                /**
                 * Will run a function and emit the duration of that function in millisecond timing to the
                 * meter provided as a Histogram metrics.
                 * @param func a function that does not return anything but will be measured.
                 * @param metricName The name of the metric that is being captured by the function.
                 * @param meter The meter making the measurement.
                 * @param attributes The attributes or dimensions associate with this measurement.
                 * @param description The description of the measurement.
                 */
                static void MakeCallWithTiming(std::function<void(void)> func,
                    Aws::String metricName,
                    const Meter &meter,
                    Aws::Map<Aws::String, Aws::String>&& attributes,
                    Aws::String description = "")
                {
                    auto before = std::chrono::steady_clock::now();
                    func();
                    auto after = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(after - before).count();
                    auto histogram = meter.CreateHistogram(std::move(metricName), MICROSECOND_METRIC_TYPE, std::move(description));
                    if (!histogram) {
                        AWS_LOG_ERROR("TracingUtil", "Failed to create histogram");
                        return;
                    }
                    histogram->record((double) duration, std::forward<Aws::Map<Aws::String, Aws::String>>(attributes));
                }

                /**
                 * Emits http metrics to a specified meter.
                 * @param metrics A http metrics collection that we will emit.
                 * @param meter The meter used for metrics emissions.
                 * @param attributes The attributes or dimensions associate with this measurement.
                 * @param description The description of the measurement.
                 */
                static void EmitCoreHttpMetrics(const Aws::Monitoring::HttpClientMetricsCollection &metrics,
                    const Meter &meter,
                    Aws::Map<Aws::String, Aws::String>&& attributes,
                    Aws::String description = "")
                {
                    for (auto const &entry: metrics) {
                        auto smithyMetric = ConvertCoreMetricToSmithy(entry.first);
                        if (smithyMetric.first != SMITHY_METRICS_UNKNOWN_METRIC) {
                            auto histogram = meter.CreateHistogram(std::move(smithyMetric.first),
                                smithyMetric.second,
                                std::move(description));
                            if (!histogram) {
                                AWS_LOG_ERROR("TracingUtil", "Failed to create histogram");
                            }
                            histogram->record((double) entry.second, attributes);
                        }
                    }
                }

                /**
                 * Converts the string Representation of a Core metric to a smithy metric.
                 * @param name the metric name.
                 * @return A tuple of metric name to measurement unit.
                 */
                static std::pair<Aws::String, Aws::String> ConvertCoreMetricToSmithy(const Aws::String &name) {
                    //TODO: Make static map, Aws::Map cannot be made static with a customer memory manager as of the moment.
                    Aws::Map<int, std::pair<Aws::String, Aws::String>> metricsTypeToName =
                        {
                            std::pair<int, std::pair<Aws::String, Aws::String>>(
                                static_cast<int>(Aws::Monitoring::HttpClientMetricsType::DnsLatency),
                                std::make_pair(SMITHY_METRICS_DNS_DURATION, MICROSECOND_METRIC_TYPE)),
                            std::pair<int, std::pair<Aws::String, Aws::String>>(
                                static_cast<int>(Aws::Monitoring::HttpClientMetricsType::ConnectLatency),
                                std::make_pair(SMITHY_METRICS_CONNECT_DURATION, MICROSECOND_METRIC_TYPE)),
                            std::pair<int, std::pair<Aws::String, Aws::String>>(
                                static_cast<int>(Aws::Monitoring::HttpClientMetricsType::SslLatency),
                                std::make_pair(SMITHY_METRICS_SSL_DURATION, MICROSECOND_METRIC_TYPE)),
                            std::pair<int, std::pair<Aws::String, Aws::String>>(
                                static_cast<int>(Aws::Monitoring::HttpClientMetricsType::DownloadSpeed),
                                std::make_pair(SMITHY_METRICS_DOWNLOAD_SPEED_METRIC, BYTES_PER_SECOND_METRIC_TYPE)),
                            std::pair<int, std::pair<Aws::String, Aws::String>>(
                                static_cast<int>(Aws::Monitoring::HttpClientMetricsType::UploadSpeed),
                                std::make_pair(SMITHY_METRICS_UPLOAD_SPEED_METRIC, BYTES_PER_SECOND_METRIC_TYPE)),
                        };

                    auto metricType = Aws::Monitoring::GetHttpClientMetricTypeByName(name);
                    auto it = metricsTypeToName.find(static_cast<int>(metricType));
                    if (it == metricsTypeToName.end()) {
                        return std::make_pair(SMITHY_METRICS_UNKNOWN_METRIC, "unknown");
                    }
                    return it->second;
                }
            };
        }
    }
}