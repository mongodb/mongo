#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Config.h>
#include <aws/crt/Exports.h>
#include <aws/crt/auth/Sigv4Signing.h>
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/iot/MqttCommon.h>

#if !BYO_CRYPTO

namespace Aws
{
    using namespace Crt::Mqtt5;

    namespace Io
    {
        class ClientBootstrap;
        class SocketOptions;
        class TlsContextOptions;
        class WebsocketConfig;
    } // namespace Io

    namespace Iot
    {

        /**
         * Class encapsulating configuration for establishing an Aws IoT Mqtt5 Connectin with custom authorizer
         */
        class AWS_CRT_CPP_API Mqtt5CustomAuthConfig
        {
          public:
            /**
             * Create a custom authorizer configuration
             */
            Mqtt5CustomAuthConfig(Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;
            virtual ~Mqtt5CustomAuthConfig();

            Mqtt5CustomAuthConfig(const Mqtt5CustomAuthConfig &rhs);
            Mqtt5CustomAuthConfig(Mqtt5CustomAuthConfig &&rhs) = delete;

            Mqtt5CustomAuthConfig &operator=(const Mqtt5CustomAuthConfig &rhs);
            Mqtt5CustomAuthConfig &operator=(Mqtt5CustomAuthConfig &&rhs) = delete;

            Mqtt5CustomAuthConfig &WithAuthorizerName(Crt::String authName);
            Mqtt5CustomAuthConfig &WithUsername(Crt::String username);
            Mqtt5CustomAuthConfig &WithPassword(Crt::ByteCursor password);
            Mqtt5CustomAuthConfig &WithTokenKeyName(Crt::String tokenKeyName);
            Mqtt5CustomAuthConfig &WithTokenValue(Crt::String tokenValue);
            Mqtt5CustomAuthConfig &WithTokenSignature(Crt::String tokenSignature);

            const Crt::Optional<Crt::String> &GetAuthorizerName();
            const Crt::Optional<Crt::String> &GetUsername();
            const Crt::Optional<Crt::ByteCursor> &GetPassword();
            const Crt::Optional<Crt::String> &GetTokenKeyName();
            const Crt::Optional<Crt::String> &GetTokenValue();
            const Crt::Optional<Crt::String> &GetTokenSignature();

          private:
            /**
             * Name of the custom authorizer to use.
             *
             * Required if the endpoint does not have a default custom authorizer associated with it.  It is strongly
             * suggested to URL-encode this value; the SDK will not do so for you.
             */
            Crt::Optional<Crt::String> m_authorizerName;

            /**
             * The username to use with the custom authorizer.  Query-string elements of this property value will be
             * unioned with the query-string elements implied by other properties in this object.
             *
             * For example, if you set this to:
             *
             * 'MyUsername?someKey=someValue'
             *
             * and use {@link authorizerName} to specify the authorizer, the final username would look like:
             *
             * `MyUsername?someKey=someValue&x-amz-customauthorizer-name=<your authorizer's name>&...`
             */
            Crt::Optional<Crt::String> m_username;

            /**
             * The password to use with the custom authorizer.  Becomes the MQTT5 CONNECT packet's password property.
             * AWS IoT Core will base64 encode this binary data before passing it to the authorizer's lambda function.
             */
            Crt::Optional<Crt::ByteCursor> m_password;

            /**
             * Key used to extract the custom authorizer token from MQTT username query-string properties.
             *
             * Required if the custom authorizer has signing enabled.  It is strongly suggested to URL-encode this
             * value; the SDK will not do so for you.
             */
            Crt::Optional<Crt::String> m_tokenKeyName;

            /**
             * An opaque token value. This value must be signed by the private key associated with the custom authorizer
             * and the result placed in the {@link tokenSignature} property.
             *
             * Required if the custom authorizer has signing enabled.
             */
            Crt::Optional<Crt::String> m_tokenValue;

            /**
             * The digital signature of the token value in the {@link tokenValue} property.  The signature must be based
             * on the private key associated with the custom authorizer.  The signature must be base64 encoded.
             *
             * Required if the custom authorizer has signing enabled.
             */
            Crt::Optional<Crt::String> m_tokenSignature;

            Crt::ByteBuf m_passwordStorage;
            Crt::Allocator *m_allocator;
        };

        /**
         * Represents a unique configuration for mqtt5 client and connection. Helps to setup Mqtt5ClientOptionsBuilder
         * for mqtt5 client.
         */
        class AWS_CRT_CPP_API Mqtt5ClientBuilder final
        {
          public:
            /**
             * Set the builder up for MTLS using certPath and pkeyPath. These are files on disk and must be in the
             * PEM format.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param certPath path to the X509 certificate (pem file) to use
             * @param pkeyPath path to the private key (pem file) to use
             * @param allocator memory allocator to use
             *
             * @return Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithMtlsFromPath(
                const Crt::String hostName,
                const char *certPath,
                const char *pkeyPath,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS using cert and pkey. These are in-memory buffers and must be in the PEM
             * format.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param certPath buffer containing the X509 certificate in a PEM format
             * @param pkeyPath buffer containing the private key in a PEM format
             * @param allocator memory allocator to use
             *
             * @return Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithMtlsFromMemory(
                const Crt::String hostName,
                const Crt::ByteCursor &certPath,
                const Crt::ByteCursor &pkeyPath,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS, using a PKCS#11 library for private key operations.
             *
             * NOTE: This only works on Unix devices.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param pkcs11Options PKCS#11 options
             * @param allocator memory allocator to use
             *
             * @return Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithMtlsPkcs11(
                const Crt::String hostName,
                const Crt::Io::TlsContextPkcs11Options &pkcs11Options,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS, using a PKCS#12 file for private key operations.
             *
             * NOTE: This only works on MacOS devices.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param options The PKCS12 options to use.
             * @param allocator - memory allocator to use
             *
             * @return Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithMtlsPkcs12(
                const Crt::String hostName,
                const struct Pkcs12Options &options,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for MTLS, using a certificate in a Windows certificate store.
             *
             * NOTE: This only works on Windows.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param windowsCertStorePath Path to certificate in a Windows certificate store.
             *    The path must use backslashes and end with the certificate's thumbprint.
             *    Example: `CurrentUser\MY\A11F8A9B5DF5B98BA3508FBCA575D09570E0D2C6`
             * @param allocator memory allocator to use
             *
             * @return Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithWindowsCertStorePath(
                const Crt::String hostName,
                const char *windowsCertStorePath,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for Websocket connection.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param config websocket configuration information
             * @param allocator memory allocator to use
             *
             * Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithWebsocket(
                const Crt::String hostName,
                const WebsocketConfig &config,
                Crt::Allocator *allocator = Crt::ApiAllocator()) noexcept;

            /**
             * Sets the builder up for connection using authorization configuration.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param customAuthConfig custom authorization configuration information
             * @param allocator memory allocator to use
             *
             * Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithCustomAuthorizer(
                const Crt::String hostName,
                const Mqtt5CustomAuthConfig &customAuthConfig,
                Crt::Allocator *allocator) noexcept;

            /**
             * Sets the builder up for connection using authorization configuration using Websockets.
             *
             * @param hostName - AWS IoT endpoint to connect to
             * @param customAuthConfig custom authorization configuration information
             * @param config websocket configuration information
             * @param allocator memory allocator to use
             *
             * Mqtt5ClientBuilder
             */
            static Mqtt5ClientBuilder *NewMqtt5ClientBuilderWithCustomAuthorizerWebsocket(
                const Crt::String hostName,
                const Mqtt5CustomAuthConfig &customAuthConfig,
                const WebsocketConfig &config,
                Crt::Allocator *allocator) noexcept;

            /**
             * Sets the host to connect to.
             *
             * @param hostname endpoint to connect to
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithHostName(Crt::String hostname);

            /**
             * Set port to connect to
             *
             * @param port port to connect to
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithPort(uint32_t port) noexcept;

            /**
             * Set booststrap for mqtt5 client
             *
             * @param bootStrap bootstrap used for mqtt5 client. The default ClientBootstrap see
             * Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap.
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithBootstrap(Crt::Io::ClientBootstrap *bootStrap) noexcept;

            /**
             * Sets the certificate authority for the endpoint you're connecting to. This is a path to a file on disk
             * and must be in PEM format.
             *
             * @param caPath path to the CA file in PEM format
             *
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithCertificateAuthority(const char *caPath) noexcept;

            /**
             * Sets the certificate authority for the endpoint you're connecting to. This is an in-memory buffer and
             * must be in PEM format.
             *
             * @param cert buffer containing the CA certificate in a PEM format
             *
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithCertificateAuthority(const Crt::ByteCursor &cert) noexcept;

            /**
             * Overrides the socket properties of the underlying MQTT connections made by the client.  Leave undefined
             * to use defaults (no TCP keep alive, 10 second socket timeout).
             *
             * @param socketOptions - The socket properties of the underlying MQTT connections made by the client
             * @return - The Mqtt5ClientBuilder
             */
            Mqtt5ClientBuilder &WithSocketOptions(Crt::Io::SocketOptions socketOptions) noexcept;

            /**
             * Sets http proxy options.
             *
             * @param proxyOptions http proxy configuration for connection establishment
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithHttpProxyOptions(
                const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept;

            /**
             * Sets the custom authorizer settings. This function will modify the username, port, and TLS options.
             *
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithCustomAuthorizer(const Iot::Mqtt5CustomAuthConfig &config) noexcept;

            /**
             * Sets mqtt5 connection options
             *
             * @param packetConnect package connection options
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithConnectOptions(std::shared_ptr<ConnectPacket> packetConnect) noexcept;

            /**
             * Sets session behavior. Overrides how the MQTT5 client should behave with respect to MQTT sessions.
             *
             * @param sessionBehavior how the MQTT5 client should behave with respect to MQTT sessions.
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithSessionBehavior(ClientSessionBehaviorType sessionBehavior) noexcept;

            /**
             * Sets client extended validation and flow control, additional controls for client behavior with
             * respect to operation validation and flow control; these checks go beyond the base MQTT5 spec to
             * respect limits of specific MQTT brokers.
             *
             * @param clientExtendedValidationAndFlowControl
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientExtendedValidationAndFlowControl(
                ClientExtendedValidationAndFlowControl clientExtendedValidationAndFlowControl) noexcept;

            /**
             * Sets OfflineQueueBehavior, controls how disconnects affect the queued and in-progress operations
             * tracked by the client.  Also controls how new operations are handled while the client is not
             * connected.  In particular, if the client is not connected, then any operation that would be failed
             * on disconnect (according to these rules) will also be rejected.
             *
             * @param offlineQueueBehavior
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithOfflineQueueBehavior(
                ClientOperationQueueBehaviorType offlineQueueBehavior) noexcept;

            /**
             * Sets ReconnectOptions. Reconnect options includes retryJitterMode, min reconnect delay time and
             * max reconnect delay time
             *
             * @param reconnectOptions
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithReconnectOptions(ReconnectOptions reconnectOptions) noexcept;

            /**
             * Sets the topic aliasing behavior that the client should use.
             *
             * @param topicAliasingOptions topic aliasing behavior options to use
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithTopicAliasingOptions(TopicAliasingOptions topicAliasingOptions) noexcept;

            /**
             * Sets minConnectedTimeToResetReconnectDelayMs, amount of time that must elapse with an established
             * connection before the reconnect delay is reset to the minimum. This helps alleviate bandwidth-waste
             * in fast reconnect cycles due to permission failures on operations.
             *
             * @param minConnectedTimeToResetReconnectDelayMs
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithMinConnectedTimeToResetReconnectDelayMs(
                uint64_t minConnectedTimeToResetReconnectDelayMs) noexcept;

            /**
             * Sets ping timeout (ms). Time interval to wait after sending a PINGREQ for a PINGRESP to arrive.
             * If one does not arrive, the client will close the current connection.
             *
             * @param pingTimeoutMs
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithPingTimeoutMs(uint32_t pingTimeoutMs) noexcept;

            /**
             * Sets Connack Timeout (ms). Time interval to wait after sending a CONNECT request for a CONNACK
             * to arrive.  If one does not arrive, the connection will be shut down.
             *
             * @param connackTimeoutMs
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithConnackTimeoutMs(uint32_t connackTimeoutMs) noexcept;

            /**
             * Sets Operation Timeout(Seconds). Time interval to wait for an ack after sending a QoS 1+ PUBLISH,
             * SUBSCRIBE, or UNSUBSCRIBE before failing the operation.
             *
             * @param ackTimeoutSec
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithAckTimeoutSec(uint32_t ackTimeoutSec) noexcept;

            /**
             * @deprecated the function is deprecated, please use `Mqtt5ClientBuilder::WithAckTimeoutSec(uint32_t)`
             *
             * Sets Operation Timeout(Seconds). Time interval to wait for an ack after sending a QoS 1+ PUBLISH,
             * SUBSCRIBE, or UNSUBSCRIBE before failing the operation.
             *
             * @param ackTimeoutSec
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithAckTimeoutSeconds(uint32_t ackTimeoutSec) noexcept;

            /**
             * Overrides the default SDK Name to send as a metric in the MQTT CONNECT packet.
             *
             * @param sdkName string to use as the SDK name parameter in the connection string
             *
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithSdkName(const Crt::String &sdkName);

            /**
             * Overrides the default SDK Version to send as a metric in the MQTT CONNECT packet.
             *
             * @param sdkVersion string to use as the SDK version parameter in the connection string
             *
             * @return this builder object
             */
            Mqtt5ClientBuilder &WithSdkVersion(const Crt::String &sdkVersion);

            /**
             * Builds a client configuration object from the set options.
             *
             * @return a new client connection config instance
             */
            std::shared_ptr<Mqtt5Client> Build() noexcept;

            /**
             * @return true if the instance is in a valid state, false otherwise.
             */
            explicit operator bool() const noexcept { return m_lastError == 0; }

            /**
             * @return the value of the last aws error encountered by operations on this instance.
             */
            int LastError() const noexcept { return m_lastError ? m_lastError : AWS_ERROR_UNKNOWN; }

            virtual ~Mqtt5ClientBuilder()
            {
                if (m_options)
                {
                    delete m_options;
                }
            };
            Mqtt5ClientBuilder(const Mqtt5ClientBuilder &) = delete;
            Mqtt5ClientBuilder(Mqtt5ClientBuilder &&) = delete;
            Mqtt5ClientBuilder &operator=(const Mqtt5ClientBuilder &) = delete;
            Mqtt5ClientBuilder &operator=(Mqtt5ClientBuilder &&) = delete;

            /**
             * Setup callback trigged when client successfully establishes an MQTT connection
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientConnectionSuccessCallback(OnConnectionSuccessHandler callback) noexcept;

            /**
             * Setup callback trigged when client fails to establish an MQTT connection
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientConnectionFailureCallback(OnConnectionFailureHandler callback) noexcept;

            /**
             * Setup callback handler trigged when client's current MQTT connection is closed
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientDisconnectionCallback(OnDisconnectionHandler callback) noexcept;

            /**
             * Setup callback handler trigged when client reaches the "Stopped" state
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientStoppedCallback(OnStoppedHandler callback) noexcept;

            /**
             * Setup callback handler trigged when client begins an attempt to connect to the remote endpoint.
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithClientAttemptingConnectCallback(OnAttemptingConnectHandler callback) noexcept;

            /**
             * Setup callback handler trigged when an MQTT PUBLISH packet is received by the client
             *
             * @param callback
             *
             * @return this option object
             */
            Mqtt5ClientBuilder &WithPublishReceivedCallback(OnPublishReceivedHandler callback) noexcept;

          private:
            // Common setup shared by all valid constructors
            Mqtt5ClientBuilder(Crt::Allocator *allocator) noexcept;
            // Common setup shared by all valid constructors
            Mqtt5ClientBuilder(int error, Crt::Allocator *allocator) noexcept;

            Crt::Allocator *m_allocator;

            /**
             * Network port of the MQTT server to connect to.
             */
            uint32_t m_port;

            /**
             * TLS context for secure socket connections.
             * If undefined, a plaintext connection will be used.
             */
            Crt::Optional<Crt::Io::TlsContextOptions> m_tlsConnectionOptions;

            /**
             * Configures (tunneling) HTTP proxy usage when establishing MQTT connections
             */
            Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;

            /**
             * Websocket related options. The clinet with use websocket for connection when set.
             */
            Crt::Optional<WebsocketConfig> m_websocketConfig;

            /**
             * Custom Authorizer Configuration
             */
            Crt::Optional<Mqtt5CustomAuthConfig> m_customAuthConfig;

            /**
             * All configurable options with respect to the CONNECT packet sent by the client, including the will.
             * These connect properties will be used for every connection attempt made by the client.
             */
            std::shared_ptr<ConnectPacket> m_connectOptions;

            Crt::Mqtt5::Mqtt5ClientOptions *m_options;

            /* Error */
            int m_lastError;

            bool m_enableMetricsCollection;

            Crt::String m_sdkName = "CPPv2";
            Crt::String m_sdkVersion = AWS_CRT_CPP_VERSION;
        };

    } // namespace Iot
} // namespace Aws

#endif // !BYO_CRYPTO
