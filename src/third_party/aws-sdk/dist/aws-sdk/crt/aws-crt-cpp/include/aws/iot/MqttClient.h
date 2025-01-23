#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Config.h>
#include <aws/crt/Exports.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/mqtt/MqttConnection.h>
#include <aws/iot/MqttCommon.h>

#if !BYO_CRYPTO

namespace Aws
{
    namespace Iot
    {
        class MqttClient;

        /**
         * Represents a unique configuration for connecting to a single AWS IoT endpoint. You can use a single instance
         * of this class PER endpoint you want to connect to. This object must live through the lifetime of your
         * connection.
         */
        class AWS_CRT_CPP_API MqttClientConnectionConfig final
        {
          public:
            static MqttClientConnectionConfig CreateInvalid(int lastError) noexcept;

            /**
             * Creates a client configuration for use with making new AWS Iot specific MQTT Connections with MTLS.
             *
             * @param endpoint endpoint to connect to
             * @param port port to connect to
             * @param socketOptions socket options to use when establishing the connection
             * @param tlsContext tls context that should be used for all connections sourced from this config
             */
            MqttClientConnectionConfig(
                const Crt::String &endpoint,
                uint32_t port,
                const Crt::Io::SocketOptions &socketOptions,
                Crt::Io::TlsContext &&tlsContext);

            /**
             * Creates a client configuration for use with making new AWS Iot specific MQTT Connections with web
             * sockets. interceptor: a callback invoked during web socket handshake giving you the opportunity to mutate
             * the request for authorization/signing purposes. If not specified, it's assumed you don't need to sign the
             * request. proxyOptions: optional, if you want to use a proxy with websockets, specify the configuration
             * options here.
             *
             * If proxy options are used, the tlsContext is applied to the connection to the remote endpoint, NOT the
             * proxy. To make a tls connection to the proxy itself, you'll want to specify tls options in proxyOptions.
             *
             * @param endpoint endpoint to connect to
             * @param port port to connect to
             * @param socketOptions socket options to use when establishing the connection
             * @param tlsContext tls context that should be used for all connections sourced from this config
             * @param interceptor websocket upgrade handshake transformation function
             * @param proxyOptions proxy configuration options
             */
            MqttClientConnectionConfig(
                const Crt::String &endpoint,
                uint32_t port,
                const Crt::Io::SocketOptions &socketOptions,
                Crt::Io::TlsContext &&tlsContext,
                Crt::Mqtt::OnWebSocketHandshakeIntercept &&interceptor,
                const Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> &proxyOptions);

            /**
             * @return true if the instance is in a valid state, false otherwise.
             */
            explicit operator bool() const noexcept { return m_context ? true : false; }

            /**
             * @return the value of the last aws error encountered by operations on this instance.
             */
            int LastError() const noexcept { return m_lastError; }

          private:
            MqttClientConnectionConfig(int lastError) noexcept;

            MqttClientConnectionConfig(
                const Crt::String &endpoint,
                uint32_t port,
                const Crt::Io::SocketOptions &socketOptions,
                Crt::Io::TlsContext &&tlsContext,
                const Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> &proxyOptions);

            Crt::String m_endpoint;
            uint32_t m_port;
            Crt::Io::TlsContext m_context;
            Crt::Io::SocketOptions m_socketOptions;
            Crt::Mqtt::OnWebSocketHandshakeIntercept m_webSocketInterceptor;
            Crt::String m_username;
            Crt::String m_password;
            Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;
            int m_lastError;

            friend class MqttClient;
            friend class MqttClientConnectionConfigBuilder;
        };

        /**
         * Represents configuration parameters for building a MqttClientConnectionConfig object. You can use a single
         * instance of this class PER MqttClientConnectionConfig you want to generate. If you want to generate a config
         * for a different endpoint or port etc... you need a new instance of this class.
         */
        class AWS_CRT_CPP_API MqttClientConnectionConfigBuilder final
        {
          public:
            MqttClientConnectionConfigBuilder();

            /**
             * Sets the builder up for MTLS using certPath and pkeyPath. These are files on disk and must be in the PEM
             * format.
             *
             * @param certPath path to the X509 certificate (pem file) to use
             * @param pkeyPath path to the private key (pem file) to use
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const char *certPath,
                const char *pkeyPath,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS using cert and pkey. These are in-memory buffers and must be in the PEM
             * format.
             *
             * @param cert buffer containing the X509 certificate in a PEM format
             * @param pkey buffer containing the private key in a PEM format
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const Crt::ByteCursor &cert,
                const Crt::ByteCursor &pkey,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS, using a PKCS#11 library for private key operations.
             *
             * NOTE: This only works on Unix devices.
             *
             * @param pkcs11Options PKCS#11 options
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const Crt::Io::TlsContextPkcs11Options &pkcs11Options,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS using a PKCS12 file and password. These are files on disk and must be in the
             * PEM format.
             *
             * NOTE: This only works on MacOS devices.
             *
             * @param options The PKCS12 options to use. Has to contain a PKCS12 filepath and password.
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const struct Pkcs12Options &options,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS, using a certificate in a Windows certificate store.
             *
             * NOTE: This only works on Windows.
             *
             * @param windowsCertStorePath Path to certificate in a Windows certificate store.
             *    The path must use backslashes and end with the certificate's thumbprint.
             *    Example: `CurrentUser\MY\A11F8A9B5DF5B98BA3508FBCA575D09570E0D2C6`
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const char *windowsCertStorePath,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for Websocket connection.
             *
             * @param config websocket configuration information
             * @param allocator memory allocator to use
             */
            MqttClientConnectionConfigBuilder(
                const WebsocketConfig &config,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Creates a new builder with default Tls options. This requires setting the connection details manually.
             *
             * @return a new builder with default Tls options
             */
            static MqttClientConnectionConfigBuilder NewDefaultBuilder() noexcept;

            /**
             * Sets endpoint to connect to.
             *
             * @param endpoint endpoint to connect to
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithEndpoint(const Crt::String &endpoint);

            /**
             * Sets endpoint to connect to.
             *
             * @param endpoint endpoint to connect to
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithEndpoint(Crt::String &&endpoint);

            /**
             * Overrides the default port. By default, if ALPN is supported, 443 will be used. Otherwise 8883 will be
             * used. If you specify 443 and ALPN is not supported, we will still attempt to connect over 443 without
             * ALPN.
             *
             * @param port port to connect to
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithPortOverride(uint32_t port) noexcept;

            /**
             * Sets the certificate authority for the endpoint you're connecting to. This is a path to a file on disk
             * and must be in PEM format.
             *
             * @param caPath path to the CA file in PEM format
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithCertificateAuthority(const char *caPath) noexcept;

            /**
             * Sets the certificate authority for the endpoint you're connecting to. This is an in-memory buffer and
             * must be in PEM format.
             *
             * @param cert buffer containing the CA certificate in a PEM format
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithCertificateAuthority(const Crt::ByteCursor &cert) noexcept;

            /**
             * TCP option: Enables TCP keep alive. Defaults to off.
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithTcpKeepAlive() noexcept;

            /**
             * TCP option: Sets the connect timeout. Defaults to 3 seconds.
             *
             * @param connectTimeoutMs socket connection timeout
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithTcpConnectTimeout(uint32_t connectTimeoutMs) noexcept;

            /**
             * TCP option: Sets time before keep alive probes are sent. Defaults to kernel defaults
             *
             * @param keepAliveTimeoutSecs time interval of no activity, in seconds, before keep alive probes
             * get sent
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithTcpKeepAliveTimeout(uint16_t keepAliveTimeoutSecs) noexcept;

            /**
             * TCP option: Sets the frequency of sending keep alive probes in seconds once the keep alive timeout
             * expires. Defaults to kernel defaults.
             *
             * @param keepAliveIntervalSecs the frequency of sending keep alive probes in seconds once the keep alive
             * timeout expires
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithTcpKeepAliveInterval(uint16_t keepAliveIntervalSecs) noexcept;

            /**
             * TCP option: Sets the amount of keep alive probes allowed to fail before the connection is terminated.
             * Defaults to kernel defaults.
             *
             * @param maxProbes the amount of keep alive probes allowed to fail before the connection is terminated
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithTcpKeepAliveMaxProbes(uint16_t maxProbes) noexcept;

            /**
             * Sets the minimum tls version that is acceptable for connection establishment
             *
             * @param minimumTlsVersion minimum tls version allowed in client connections
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithMinimumTlsVersion(aws_tls_versions minimumTlsVersion) noexcept;

            /**
             * Sets http proxy options.
             *
             * @param proxyOptions proxy configuration options for connection establishment
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithHttpProxyOptions(
                const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept;

            /**
             * Whether to send the SDK name and version number in the MQTT CONNECT packet.
             * Default is True.
             *
             * @param enabled true to send SDK version/name in the connect for metrics gathering purposes
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithMetricsCollection(bool enabled);

            /**
             * Overrides the default SDK Name to send as a metric in the MQTT CONNECT packet.
             *
             * @param sdkName string to use as the SDK name parameter in the connection string
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithSdkName(const Crt::String &sdkName);

            /**
             * Overrides the default SDK Version to send as a metric in the MQTT CONNECT packet.
             *
             * @param sdkVersion string to use as the SDK version parameter in the connection string
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithSdkVersion(const Crt::String &sdkVersion);

            /**
             * Sets the custom authorizer settings. This function will modify the username, port, and TLS options.
             *
             * @param username The username to use with the custom authorizer. If an empty string is passed, it will
             *                 check to see if a username has already been set (via WithUsername function). If no
             *                 username is set then no username will be passed with the MQTT connection.
             * @param authorizerName The name of the custom authorizer. If an empty string is passed, then
             *                       'x-amz-customauthorizer-name' will not be added with the MQTT connection.
             * @param authorizerSignature The signature of the custom authorizer.
             *                            NOTE: This will NOT work without the token key name and token value, which
             * requires using the non-depreciated API.
             * @param password The password to use with the custom authorizer. If null is passed, then no password will
             *                 be set.
             *
             * @deprecated Please use the full WithCustomAuthorizer function that includes `tokenKeyName` and
             *             `tokenValue`. This version is left for backwards compatibility purposes.
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithCustomAuthorizer(
                const Crt::String &username,
                const Crt::String &authorizerName,
                const Crt::String &authorizerSignature,
                const Crt::String &password) noexcept;

            /**
             * Sets the custom authorizer settings. This function will modify the username, port, and TLS options.
             *
             * @param username The username to use with the custom authorizer. If an empty string is passed, it will
             *                 check to see if a username has already been set (via WithUsername function). If no
             *                 username is set then no username will be passed with the MQTT connection.
             * @param authorizerName The name of the custom authorizer. If an empty string is passed, then
             *                       'x-amz-customauthorizer-name' will not be added with the MQTT connection.
             * @param authorizerSignature The signature of the custom authorizer. If an empty string is passed, then
             *                            'x-amz-customauthorizer-signature' will not be added with the MQTT connection.
             *                            The signature must be based on the private key associated with the custom
             *                            authorizer. The signature must be base64 encoded.
             * @param password The password to use with the custom authorizer. If null is passed, then no password will
             *                 be set.
             * @param tokenKeyName Used to extract the custom authorizer token from MQTT username query-string
             *                     properties. Required if the custom authorizer has signing enabled. It is strongly
             *                     suggested to URL encode this value; the SDK will not do so for you.
             * @param tokenValue An opaque token value. Required if the custom authorizer has signing enabled. This
             *                   value must be signed by the private key associated with the custom authorizer and
             *                   the result placed in the authorizerSignature argument.
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithCustomAuthorizer(
                const Crt::String &username,
                const Crt::String &authorizerName,
                const Crt::String &authorizerSignature,
                const Crt::String &password,
                const Crt::String &tokenKeyName,
                const Crt::String &tokenValue) noexcept;

            /**
             * Sets username for the connection
             *
             * @param username the username that will be passed with the MQTT connection
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithUsername(const Crt::String &username) noexcept;

            /**
             * Sets password for the connection
             *
             * @param password the password that will be passed with the MQTT connection
             *
             * @return this builder object
             */
            MqttClientConnectionConfigBuilder &WithPassword(const Crt::String &password) noexcept;

            /**
             * Builds a client configuration object from the set options.
             *
             * @return a new client connection config instance
             */
            MqttClientConnectionConfig Build() noexcept;

            /**
             * @return true if the instance is in a valid state, false otherwise.
             */
            explicit operator bool() const noexcept { return m_lastError == 0; }

            /**
             * @return the value of the last aws error encountered by operations on this instance.
             */
            int LastError() const noexcept { return m_lastError ? m_lastError : AWS_ERROR_UNKNOWN; }

          private:
            // Common setup shared by all valid constructors
            MqttClientConnectionConfigBuilder(Crt::Allocator *allocator) noexcept;

            // Helper function to add parameters to the username in the WithCustomAuthorizer function
            Crt::String AddToUsernameParameter(
                Crt::String currentUsername,
                Crt::String parameterValue,
                Crt::String parameterPreText);

            Crt::Allocator *m_allocator;
            Crt::String m_endpoint;
            uint32_t m_portOverride;
            Crt::Io::SocketOptions m_socketOptions;
            Crt::Io::TlsContextOptions m_contextOptions;
            Crt::Optional<WebsocketConfig> m_websocketConfig;
            Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;
            bool m_enableMetricsCollection = true;
            Crt::String m_sdkName = "CPPv2";
            Crt::String m_sdkVersion;
            Crt::String m_username = "";
            Crt::String m_password = "";
            bool m_isUsingCustomAuthorizer = false;

            int m_lastError;
        };

        /**
         * AWS IOT specific Mqtt Client. Sets defaults for using the AWS IOT service. You'll need an instance of
         * MqttClientConnectionConfig to use. Once NewConnection returns, you use it's return value identically
         * to how you would use Aws::Crt::Mqtt::MqttConnection
         */
        class AWS_CRT_CPP_API MqttClient final
        {
          public:
            MqttClient(Crt::Io::ClientBootstrap &bootstrap, Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Constructs a new Mqtt Client object using the static default ClientBootstrap.
             *
             * For more information on the default ClientBootstrap see
             * Aws::Crt::ApiHandle::GetOrCreateDefaultClientBootstrap
             */
            MqttClient(Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Creates a new mqtt connection from a connection configuration object
             * @param config mqtt connection configuration
             * @return a new mqtt connection
             */
            std::shared_ptr<Crt::Mqtt::MqttConnection> NewConnection(const MqttClientConnectionConfig &config) noexcept;

            /**
             * @return the value of the last aws error encountered by operations on this instance.
             */
            int LastError() const noexcept { return m_client.LastError(); }

            /**
             * @return true if the instance is in a valid state, false otherwise.
             */
            explicit operator bool() const noexcept { return m_client ? true : false; }

          private:
            Crt::Mqtt::MqttClient m_client;
            int m_lastError;
        };
    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
