/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/HashingUtils.h>
#include <aws/core/monitoring/HttpClientMetrics.h>

namespace Aws
{
    namespace Monitoring
    {
        static const char HTTP_CLIENT_METRICS_DESTINATION_IP[] = "DestinationIp";
        static const char HTTP_CLIENT_METRICS_ACQUIRE_CONNECTION_LATENCY[] = "AcquireConnectionLatency";
        static const char HTTP_CLIENT_METRICS_CONNECTION_REUSED[] = "ConnectionReused";
        static const char HTTP_CLIENT_METRICS_CONNECTION_LATENCY[] = "ConnectLatency";
        static const char HTTP_CLIENT_METRICS_REQUEST_LATENCY[] = "RequestLatency";
        static const char HTTP_CLIENT_METRICS_DNS_LATENCY[] = "DnsLatency";
        static const char HTTP_CLIENT_METRICS_TCP_LATENCY[] = "TcpLatency";
        static const char HTTP_CLIENT_METRICS_SSL_LATENCY[] = "SslLatency";
        static const char HTTP_CLIENT_METRICS_THROUGHPUT[] = "Throughput";
        static const char HTTP_CLIENT_METRICS_DOWNLOAD_SPEED[] = "DownloadSpeed";
        static const char HTTP_CLIENT_METRICS_UPLOAD_SPEED[] = "UploadSpeed";
        static const char HTTP_CLIENT_METRICS_UNKNOWN[] = "Unknown";

        using namespace Aws::Utils;
        HttpClientMetricsType GetHttpClientMetricTypeByName(const Aws::String& name)
        {
            //TODO: Make static map, Aws::Map cannot be made static with a customer memory manager as of the moment.
            Aws::Map<int, HttpClientMetricsType> metricsNameHashToType =
            {
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_DESTINATION_IP), HttpClientMetricsType::DestinationIp),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_ACQUIRE_CONNECTION_LATENCY), HttpClientMetricsType::AcquireConnectionLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_CONNECTION_REUSED), HttpClientMetricsType::ConnectionReused),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_CONNECTION_LATENCY), HttpClientMetricsType::ConnectLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_REQUEST_LATENCY), HttpClientMetricsType::RequestLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_DNS_LATENCY), HttpClientMetricsType::DnsLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_TCP_LATENCY), HttpClientMetricsType::TcpLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_SSL_LATENCY), HttpClientMetricsType::SslLatency),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_THROUGHPUT), HttpClientMetricsType::Throughput),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_DOWNLOAD_SPEED), HttpClientMetricsType::DownloadSpeed),
                std::pair<int, HttpClientMetricsType>(HashingUtils::HashString(HTTP_CLIENT_METRICS_UPLOAD_SPEED), HttpClientMetricsType::UploadSpeed),
            };

            int nameHash = HashingUtils::HashString(name.c_str());
            auto it = metricsNameHashToType.find(nameHash);
            if (it == metricsNameHashToType.end())
            {
                return HttpClientMetricsType::Unknown;
            }
            return it->second;
        }

        Aws::String GetHttpClientMetricNameByType(HttpClientMetricsType type)
        {
            //TODO: Make static map, Aws::Map cannot be made static with a customer memory manager as of the moment.
            Aws::Map<int, Aws::String> metricsTypeToName =
            {
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::DestinationIp), HTTP_CLIENT_METRICS_DESTINATION_IP),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::AcquireConnectionLatency), HTTP_CLIENT_METRICS_ACQUIRE_CONNECTION_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::ConnectionReused), HTTP_CLIENT_METRICS_CONNECTION_REUSED),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::ConnectLatency), HTTP_CLIENT_METRICS_CONNECTION_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::RequestLatency), HTTP_CLIENT_METRICS_REQUEST_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::DnsLatency), HTTP_CLIENT_METRICS_DNS_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::TcpLatency), HTTP_CLIENT_METRICS_TCP_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::SslLatency), HTTP_CLIENT_METRICS_SSL_LATENCY),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::Throughput), HTTP_CLIENT_METRICS_THROUGHPUT),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::DownloadSpeed), HTTP_CLIENT_METRICS_DOWNLOAD_SPEED),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::UploadSpeed), HTTP_CLIENT_METRICS_UPLOAD_SPEED),
                std::pair<int, Aws::String>(static_cast<int>(HttpClientMetricsType::Unknown), HTTP_CLIENT_METRICS_UNKNOWN),
            };

            auto it = metricsTypeToName.find(static_cast<int>(type));
            if (it == metricsTypeToName.end())
            {
                return HTTP_CLIENT_METRICS_UNKNOWN;
            }
            return Aws::String(it->second.c_str());
        }

    }
}
