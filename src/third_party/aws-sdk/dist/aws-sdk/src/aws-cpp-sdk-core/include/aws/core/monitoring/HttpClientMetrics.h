/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSMap.h>

namespace Aws
{
    namespace Monitoring
    {
        /**
         * Metrics definitions optional from HttpClient module inside AWS Sdk core.
         */
        enum class HttpClientMetricsType
        {
            /*
             * Requires a successful DNS lookup, contains the (IPv4 or IPv6 as appropriate) numeric address of the connection endpoint to which the attempt was sent.
             */
            DestinationIp = 0,

            /**
             * Requires the SDK to recognize that an existing connection was reused to make the request,
             * contains the time interval (in milliseconds) between the construction of the http request and when a connection was successfully acquired from the connection pool.
             */
            AcquireConnectionLatency,

            /**
             * Requires the SDK to recognize whether or not an existing connection was used to make the request,
             * contains 1 if an existing connection was used to perform the http request; contains 0 if a new connection was opened to perform the http request.
             */
            ConnectionReused,

            /**
             * Requires the SDK to recognize that a new connection was established to make the request,
             * contains the time interval (in milliseconds) between the construction of the http request and when a connection was fully established.
             * If the SDK is able to estimate this time despite not having a perfectly accurate callback for the specific event, then it should.
             * For example, if the http client includes user-level data write functions that are guaranteed to be called shortly after connection establishment,
             * then the first call could be used as a reasonable time marker for ConnectLatency
             */
            ConnectLatency,

            /**
             * Requires the SDK to be able to mark the point in time where the request starts transmission,
             * contains the time interval (in milliseconds) between when the request begins transmission to the service and when a terminal error has occurred or the response has been parsed,
             * excluding streaming payloads. Like ConnectLatency, if the SDK has access to an event that is a "close enough" marker in time, it should include this entry.
             * The request here is a http level request, not a service level API request.
             */
            RequestLatency,

            /**
             * Requires the SDK to have access to how long it took to perform DNS lookup, if it took place,
             * contains the time (in milliseconds) it took to perform DNS lookup, during the Api Call attempt.
             */
            DnsLatency,

            /**
             * Requires the SDK to have access to how long it took to establish the underlying Tcp/Ip connection used for the request,
             * contains the time (in milliseconds) it took to fully establish the TCP/IP connection used to make the request attempt.
             */
            TcpLatency,

            /**
             * Requires the SDK to have access to how long it took to perform the SSL handshake for a secure request,
             * contains the time (in milliseconds) it took to perform a SSL handshake over the established TCP/IP connection.
             */
            SslLatency,

            /**
             * Request the SDK to have access to the download speed in bytes per
             * second for a request.
             */
             DownloadSpeed,
             Throughput,

             /**
              * Upload speed of the request in bytes per second.
              */
             UploadSpeed,

            /**
             * Unknow Metrics Type
             */
            Unknown
        };

        typedef Aws::Map<Aws::String, int64_t> HttpClientMetricsCollection;

        AWS_CORE_API HttpClientMetricsType GetHttpClientMetricTypeByName(const Aws::String& name);

        AWS_CORE_API Aws::String GetHttpClientMetricNameByType(HttpClientMetricsType type);
    }
}
