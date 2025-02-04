/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/http/Scheme.h>
#include <aws/core/http/Version.h>
#include <aws/core/Region.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/utils/Array.h>
#include <aws/crt/Optional.h>
#include <smithy/tracing/TelemetryProvider.h>
#include <memory>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            class Executor;
        } // namespace Threading

        namespace RateLimits
        {
            class RateLimiterInterface;
        } // namespace RateLimits
    } // namespace Utils
    namespace Client
    {
        class RetryStrategy; // forward declare

        /**
         * Sets the behaviors of the underlying HTTP clients handling response with 30x status code.
         * By default, HTTP clients will always redirect the 30x response automatically, except when
         * specifying aws-global as the client region, then SDK will handle 30x response and redirect
         * the request manually.
         */
        enum class FollowRedirectsPolicy
        {
            DEFAULT,
            ALWAYS,
            NEVER
        };

        /**
         * This setting is an enumeration, not a boolean, to allow for future expansion.
         */
        enum class UseRequestCompression
        {
          DISABLE,
          ENABLE,
        };

        struct RequestCompressionConfig {
          UseRequestCompression useRequestCompression=UseRequestCompression::ENABLE;
          size_t requestMinCompressionSizeBytes = 10240;
        };
         /**
          * This structure is used to provide initial configuration values to the default ClientConfiguration constructor for the following parameter(s):
          * - disableIMDS
         */
        struct ClientConfigurationInitValues {
            bool shouldDisableIMDS = false;
        };

        /**
         * This mutable structure is used to configure any of the AWS clients.
         * Default values can only be overwritten prior to passing to the client constructors.
         */
        struct AWS_CORE_API ClientConfiguration
        {
            struct ProviderFactories
            {
                /**
                 * Retry Strategy factory method. Default is DefaultRetryStrategy (i.e. exponential backoff).
                 */
                std::function<std::shared_ptr<RetryStrategy>()> retryStrategyCreateFn;
                /**
                 * Threading Executor factory method. Default creates a factory that creates DefaultExecutor
                 *  (i.e. spawn a separate thread for each task) for backward compatibility reasons.
                 *  Please switch to a better executor such as PooledThreadExecutor.
                 */
                std::function<std::shared_ptr<Utils::Threading::Executor>()> executorCreateFn;
                /**
                 * Rate Limiter factory for outgoing bandwidth. Default is wide-open.
                 */
                std::function<std::shared_ptr<Utils::RateLimits::RateLimiterInterface>()> writeRateLimiterCreateFn;
                /**
                 * Rate Limiter factory for incoming bandwidth. Default is wide-open.
                 */
                std::function<std::shared_ptr<Utils::RateLimits::RateLimiterInterface>()> readRateLimiterCreateFn;
                /**
                 * TelemetryProvider factory. Defaults to Noop provider.
                 */
                std::function<std::shared_ptr<smithy::components::tracing::TelemetryProvider>()> telemetryProviderCreateFn;

                static ProviderFactories defaultFactories;
            };

            ClientConfiguration();

            /**
             * Create a configuration with default settings. By default IMDS calls are enabled.
             * @param ClientConfigurationInitValues ClientConfiguration initial customizable values
             */
            ClientConfiguration(const ClientConfigurationInitValues &configuration);

            /**
             * Create a configuration based on settings in the aws configuration file for the given profile name.
             * The configuration file location can be set via the environment variable AWS_CONFIG_FILE
             * @param profileName the aws profile name.
             * @param shouldDisableIMDS whether or not to disable IMDS calls.
             */
            ClientConfiguration(const char* profileName, bool shouldDisableIMDS = false);

            /**
             * Create a configuration with a predefined smart defaults
             * @param useSmartDefaults, required to differentiate c-tors
             * @param defaultMode, default mode to use
             * @param shouldDisableIMDS whether or not to disable IMDS calls.
             */
            explicit ClientConfiguration(bool useSmartDefaults, const char* defaultMode = "legacy", bool shouldDisableIMDS = false);

            /**
             * Add virtual method to allow use of dynamic_cast under inheritance.
             */
            virtual ~ClientConfiguration() = default;

            /**
             * Client configuration factory methods to init client utility classes such as Executor, Retry Strategy
             */
            ProviderFactories configFactories = ProviderFactories::defaultFactories;

            /**
             * User Agent string user for http calls. This is filled in for you in the constructor. Don't override this unless you have a really good reason.
             */
            Aws::String userAgent;
            /**
             * Http scheme to use. E.g. Http or Https. Default HTTPS
             */
            Aws::Http::Scheme scheme;
            /**
             * AWS Region to use in signing requests. Default US_EAST_1
             */
            Aws::String region;
            /**
             * Use dual stack endpoint in the endpoint calculation. It is your responsibility to verify that the service supports ipv6 in the region you select.
             */
            bool useDualStack = false;

            /**
             * Use FIPS endpoint in the endpoint calculation. Please check first that the service supports FIPS in a selected region.
             */
            bool useFIPS = false;

            /**
             * Max concurrent tcp connections for a single http client to use. Default 25.
             */
            unsigned maxConnections = 25;
            /**
             * This is currently only applicable for Curl to set the http request level timeout, including possible dns lookup time, connection establish time, ssl handshake time and actual data transmission time.
             * the corresponding Curl option is CURLOPT_TIMEOUT_MS
             * defaults to 0, no http request level timeout.
             */
            long httpRequestTimeoutMs = 0;
            /**
             * Socket read timeouts for HTTP clients on Windows. Default 3000 ms. This should be more than adequate for most services. However, if you are transferring large amounts of data
             * or are worried about higher latencies, you should set to something that makes more sense for your use case.
             * For Curl, it's the low speed time, which contains the time in number milliseconds that transfer speed should be below "lowSpeedLimit" for the library to consider it too slow and abort.
             * Note that for Curl this config is converted to seconds by rounding down to the nearest whole second except when the value is greater than 0 and less than 1000. In this case it is set to one second. When it's 0, low speed limit check will be disabled.
             * Note that for Windows when this config is 0, the behavior is not specified by Windows.
             */
            long requestTimeoutMs = 0;
            /**
             * Socket connect timeout. Default 1000 ms. Unless you are very far away from your the data center you are talking to, 1000ms is more than sufficient.
             */
            long connectTimeoutMs = 1000;
            /**
             * Enable TCP keep-alive. Default true;
             * No-op for WinHTTP, WinINet and IXMLHTTPRequest2 client.
             */
            bool enableTcpKeepAlive = true;
            /**
             * Interval to send a keep-alive packet over the connection. Default 30 seconds. Minimum 15 seconds.
             * WinHTTP & libcurl support this option. Note that for Curl, this value will be rounded to an integer with second granularity.
             * No-op for WinINet and IXMLHTTPRequest2 client.
             */
            unsigned long tcpKeepAliveIntervalMs = 30000;
            /**
             * Average transfer speed in bytes per second that the transfer should be below during the request timeout interval for it to be considered too slow and abort.
             * Default 1 byte/second. Only for CURL client currently.
             */
            unsigned long lowSpeedLimit = 1;
            /**
             * Strategy to use in case of failed requests. Default is DefaultRetryStrategy (i.e. exponential backoff).
             * Provide retry strategy here or via a factory method.
             */
            std::shared_ptr<RetryStrategy> retryStrategy = nullptr;
            /**
             * Override the http endpoint used to talk to a service.
             */
            Aws::String endpointOverride;

            /**
             * Allow HTTP client to discover system proxy setting. Off by default for legacy reasons.
             */
            bool allowSystemProxy = false;
            /**
             * If you have users going through a proxy, set the proxy scheme here. Default HTTP
             */
            Aws::Http::Scheme proxyScheme;
            /**
             * If you have users going through a proxy, set the host here.
             */
            Aws::String proxyHost;
            /**
             * If you have users going through a proxy, set the port here.
             */
            unsigned proxyPort = 0;
            /**
             * If you have users going through a proxy, set the username here.
             */
            Aws::String proxyUserName;
            /**
            * If you have users going through a proxy, set the password here.
            */
            Aws::String proxyPassword;
            /**
            * SSL Certificate file to use for connecting to an HTTPS proxy.
            * Used to set CURLOPT_PROXY_SSLCERT in libcurl. Example: client.pem
            */
            Aws::String proxySSLCertPath;
            /**
            * Type of proxy client SSL certificate.
            * Used to set CURLOPT_PROXY_SSLCERTTYPE in libcurl. Example: PEM
            */
            Aws::String proxySSLCertType;
            /**
            * Private key file to use for connecting to an HTTPS proxy.
            * Used to set CURLOPT_PROXY_SSLKEY in libcurl. Example: key.pem
            */
            Aws::String proxySSLKeyPath;
            /**
            * Type of private key file used to connect to an HTTPS proxy.
            * Used to set CURLOPT_PROXY_SSLKEYTYPE in libcurl. Example: PEM
            */
            Aws::String proxySSLKeyType;
            /**
            * Passphrase to the private key file used to connect to an HTTPS proxy.
            * Used to set CURLOPT_PROXY_KEYPASSWD in libcurl. Example: password1
            */
            Aws::String proxySSLKeyPassword;
            /**
            * Calls to hosts in this vector will not use proxy configuration
            */
            Aws::Utils::Array<Aws::String> nonProxyHosts;
            /**
             * Threading Executor implementation. Default uses std::thread::detach()
             * Provide executor here or via a factory method.
             */
            std::shared_ptr<Aws::Utils::Threading::Executor> executor = nullptr;
            /**
             * If you need to test and want to get around TLS validation errors, do that here.
             * You probably shouldn't use this flag in a production scenario.
             */
            bool verifySSL = true;
            /**
             * If your Certificate Authority path is different from the default, you can tell
             * clients that aren't using the default trust store where to find your CA trust store.
             * If you are on windows or apple, you likely don't want this.
             */
            Aws::String caPath;
            /**
             * Same as caPath, but used when verifying an HTTPS proxy. 
             * Used to set CURLOPT_PROXY_CAPATH in libcurl and proxy tls
             * settings in crt HTTP client.
             * Does nothing on windows.
             */
            Aws::String proxyCaPath;
            /**
             * If you certificate file is different from the default, you can tell clients that
             * aren't using the default trust store where to find your ca file.
             * If you are on windows or apple, you likely don't want this.
             */
             Aws::String caFile;
            /**
             * Same as caFile, but used when verifying an HTTPS proxy. 
             * Used to set CURLOPT_PROXY_CAINFO in libcurl and proxy tls
             * settings in crt HTTP client.
             * Does nothing on windows.
             */
            Aws::String proxyCaFile;
            /**
             * Rate Limiter implementation for outgoing bandwidth. Default is wide-open.
             * Provide limiter here or via a factory method.
             */
            std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> writeRateLimiter = nullptr;
            /**
            * Rate Limiter implementation for incoming bandwidth. Default is wide-open.
            * Provide limiter here or via a factory method.
            */
            std::shared_ptr<Aws::Utils::RateLimits::RateLimiterInterface> readRateLimiter = nullptr;
            /**
             * Override the http implementation the default factory returns.
             */
            Aws::Http::TransferLibType httpLibOverride;
            /**
             * Configure low latency or low cpu consumption http client operation mode.
             * Currently applies only to streaming APIs and libCurl. Defaults to LOW_LATENCY
             */
            Aws::Http::TransferLibPerformanceMode httpLibPerfMode = Http::TransferLibPerformanceMode::LOW_LATENCY;
            /**
             * Sets the behavior how http stack handles 30x redirect codes.
             */
            FollowRedirectsPolicy followRedirects;

            /**
             * Only works for Curl http client.
             * Curl will by default add "Expect: 100-Continue" header in a Http request so as to avoid sending http
             * payload to wire if server respond error immediately after receiving the header.
             * Set this option to true will tell Curl to send http request header and body together.
             * This can save one round-trip time and especially useful when the payload is small and network latency is more important.
             * But be careful when Http request has large payload such S3 PutObject. You don't want to spend long time sending a large payload just getting a error response for server.
             * The default value will be false.
             */
            bool disableExpectHeader = false;

            /**
             * If set to true clock skew will be adjusted after each http attempt, default to true.
             */
            bool enableClockSkewAdjustment = true;

            /**
             * Enable host prefix injection.
             * For services whose endpoint is injectable. e.g. servicediscovery, you can modify the http host's prefix so as to add "data-" prefix for DiscoverInstances request.
             * Default to true, enabled. You can disable it for testing purpose.
             *
             * Deprecated in API v. 1.10. Please set in service-specific client configuration.
             */
            bool enableHostPrefixInjection = true;

            /**
             * Enable endpoint discovery
             * For some services to dynamically set up their endpoints for different requests.
             * By default, service clients will decide if endpoint discovery is enabled or not.
             * If disabled, regional or overridden endpoint will be used instead.
             * If a request requires endpoint discovery but you disabled it. The request will never succeed.
             * A boolean value is either true of false, use Optional here to have an instance does not contain a value,
             * such that SDK will decide the default behavior as stated before, if no value specified.
             *
             * Deprecated in API v. 1.10. Please set in service-specific client configuration.
             */
            Aws::Crt::Optional<bool> enableEndpointDiscovery;

            /**
             * Enable http client (WinHTTP or CURL) traces.
             * Defaults to false, it's an optional feature.
             */
            bool enableHttpClientTrace = false;

            /**
             * profileName in config file that will be used by this object to resolve more configurations.
             */
            Aws::String profileName;

            /**
             * Request compression configuration
             * To use this feature, the service needs to provide the support, and the compression
             * algorithms needs to be available at SDK build time.
             */
            Aws::Client::RequestCompressionConfig requestCompressionConfig;

            /**
             * Disable all internal IMDS Calls
             */
            bool disableIMDS = false;

            /**
             * Request HTTP client to use specific http version. Currently supported for
             * only Curl. More or less is a one to one conversion of the CURLOPT_HTTP_VERSION
             * configuration option.
             *
             * Default to Version 2 TLS which is the default after curl version 7.62.0. Will
             * fall back to 1.1 if compiled against a earlier version of curl.
             */
            Aws::Http::Version version = Http::Version::HTTP_VERSION_2TLS;

            /**
             * Disable all internal IMDSV1 Calls
             */
            bool disableImdsV1 = false;

            /**
             * AppId is an optional application specific identifier that can be set.
             * When set it will be appended to the User-Agent header of every request
             * in the form of App/{AppId}. This variable is sourced from environment
             * variable AWS_SDK_UA_APP_ID or the shared config profile attribute sdk_ua_app_id.
             * See https://docs.aws.amazon.com/sdkref/latest/guide/settings-reference.html for
             * more information on environment variables and shared config settings.
             */
            Aws::String appId;

            /**
             * A helper function to read config value from env variable or aws profile config
             */
            static Aws::String LoadConfigFromEnvOrProfile(const Aws::String& envKey,
                                                          const Aws::String& profile,
                                                          const Aws::String& profileProperty,
                                                          const Aws::Vector<Aws::String>& allowedValues,
                                                          const Aws::String& defaultValue);

            /**
             * A wrapper for interfacing with telemetry functionality. Defaults to Noop provider.
             * Provide TelemetryProvider here or via a factory method.
             */
            std::shared_ptr<smithy::components::tracing::TelemetryProvider> telemetryProvider;
        };

        /**
         * A helper function to initialize a retry strategy.
         * Default is DefaultRetryStrategy (i.e. exponential backoff)
         */
        std::shared_ptr<RetryStrategy> InitRetryStrategy(Aws::String retryMode = "");

        /**
         * A helper function to compute a user agent
         * @return Aws::String with a user-agent
         */
        AWS_CORE_API Aws::String ComputeUserAgentString(ClientConfiguration const * const pConfig = nullptr);

        AWS_CORE_API Aws::String FilterUserAgentToken(char const * const token);

    } // namespace Client
} // namespace Aws
