#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/mqtt/Mqtt5Types.h>
#include <aws/crt/mqtt/MqttClient.h>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            class ConnectPacket;
            class ConnAckPacket;
            class DisconnectPacket;
            class Mqtt5Client;
            class Mqtt5ClientOptions;
            class NegotiatedSettings;
            class PublishResult;
            class PublishPacket;
            class PubAckPacket;
            class SubscribePacket;
            class SubAckPacket;
            class UnsubscribePacket;
            class UnSubAckPacket;
            class Mqtt5ClientCore;

            class Mqtt5to3AdapterOptions;

            /**
             * An enumeration that controls how the client applies topic aliasing to outbound publish packets.
             *
             * Topic alias behavior is described in
             * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113
             */
            enum class OutboundTopicAliasBehaviorType
            {

                /**
                 * Maps to Disabled.  This keeps the client from being broken (by default) if the broker
                 * topic aliasing implementation has a problem.
                 */
                Default = AWS_MQTT5_COTABT_DEFAULT,

                /**
                 * Outbound aliasing is the user's responsibility.  Client will cache and use
                 * previously-established aliases if they fall within the negotiated limits of the connection.
                 *
                 * The user must still always submit a full topic in their publishes because disconnections disrupt
                 * topic alias mappings unpredictably.  The client will properly use a requested alias when the
                 * most-recently-seen binding for a topic alias value matches the alias and topic in the publish packet.
                 */
                Manual = AWS_MQTT5_COTABT_MANUAL,

                /**
                 * (Recommended) The client will ignore any user-specified topic aliasing and instead use an LRU cache
                 * to drive alias usage.
                 */
                LRU = AWS_MQTT5_COTABT_LRU,

                /**
                 * Completely disable outbound topic aliasing.
                 */
                Disabled = AWS_MQTT5_COTABT_DISABLED,
            };

            /**
             * An enumeration that controls whether or not the client allows the broker to send publishes that use topic
             * aliasing.
             *
             * Topic alias behavior is described in
             * https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113
             */
            enum class InboundTopicAliasBehaviorType
            {

                /**
                 * Maps to Disabled.  This keeps the client from being broken (by default) if the broker
                 * topic aliasing implementation has a problem.
                 */
                Default = AWS_MQTT5_CITABT_DEFAULT,

                /**
                 * Allow the server to send PUBLISH packets to the client that use topic aliasing
                 */
                Enabled = AWS_MQTT5_CITABT_ENABLED,

                /**
                 * Forbid the server from sending PUBLISH packets to the client that use topic aliasing
                 */
                Disabled = AWS_MQTT5_CITABT_DISABLED,
            };

            /**
             * Configuration for all client topic aliasing behavior.
             */
            struct AWS_CRT_CPP_API TopicAliasingOptions
            {

                /**
                 * Controls what kind of outbound topic aliasing behavior the client should attempt to use.
                 *
                 * If topic aliasing is not supported by the server, this setting has no effect and any attempts to
                 * directly manipulate the topic alias id in outbound publishes will be ignored.
                 *
                 * If left undefined, then outbound topic aliasing is disabled.
                 */
                Crt::Optional<OutboundTopicAliasBehaviorType> m_outboundBehavior;

                /**
                 * If outbound topic aliasing is set to LRU, this controls the maximum size of the cache.  If outbound
                 * topic aliasing is set to LRU and this is zero or undefined, a sensible default is used (25).  If
                 * outbound topic aliasing is not set to LRU, then this setting has no effect.
                 *
                 * The final size of the cache is determined by the minimum of this setting and the value of the
                 * topic_alias_maximum property of the received CONNACK.  If the received CONNACK does not have an
                 * explicit positive value for that field, outbound topic aliasing is disabled for the duration of that
                 * connection.
                 */
                Crt::Optional<uint16_t> m_outboundCacheMaxSize;

                /**
                 * Controls whether or not the client allows the broker to use topic aliasing when sending publishes.
                 * Even if inbound topic aliasing is enabled, it is up to the server to choose whether or not to use it.
                 *
                 * If left undefined, then inbound topic aliasing is disabled.
                 */
                Crt::Optional<InboundTopicAliasBehaviorType> m_inboundBehavior;

                /**
                 * If inbound topic aliasing is enabled, this will control the size of the inbound alias cache.  If
                 * inbound aliases are enabled and this is zero or undefined, then a sensible default will be used (25).
                 * If inbound aliases are disabled, this setting has no effect.
                 *
                 * Behaviorally, this value overrides anything present in the topic_alias_maximum field of
                 * the CONNECT packet options.
                 */
                Crt::Optional<uint16_t> m_inboundCacheMaxSize;
            };

            struct AWS_CRT_CPP_API ReconnectOptions
            {
                /**
                 * Controls how the reconnect delay is modified in order to smooth out the distribution of reconnection
                 * attempt timepoints for a large set of reconnecting clients.
                 */
                ExponentialBackoffJitterMode m_reconnectMode;

                /**
                 * Minimum amount of time to wait to reconnect after a disconnect.  Exponential backoff is performed
                 * with jitter after each connection failure.
                 */
                uint64_t m_minReconnectDelayMs;

                /**
                 * Maximum amount of time to wait to reconnect after a disconnect.  Exponential backoff is performed
                 * with jitter after each connection failure.
                 */
                uint64_t m_maxReconnectDelayMs;

                /**
                 * Amount of time that must elapse with an established connection before the reconnect delay is reset to
                 * the minimum. This helps alleviate bandwidth-waste in fast reconnect cycles due to permission failures
                 * on operations.
                 */
                uint64_t m_minConnectedTimeToResetReconnectDelayMs;
            };

            /**
             * Simple statistics about the current state of the client's queue of operations
             */
            struct AWS_CRT_CPP_API Mqtt5ClientOperationStatistics
            {
                /**
                 * total number of operations submitted to the client that have not yet been completed.  Unacked
                 * operations are a subset of this.
                 */
                uint64_t incompleteOperationCount;

                /**
                 * total packet size of operations submitted to the client that have not yet been completed.  Unacked
                 * operations are a subset of this.
                 */
                uint64_t incompleteOperationSize;

                /**
                 * total number of operations that have been sent to the server and are waiting for a corresponding ACK
                 * before they can be completed.
                 */
                uint64_t unackedOperationCount;

                /**
                 * total packet size of operations that have been sent to the server and are waiting for a corresponding
                 * ACK before they can be completed.
                 */
                uint64_t unackedOperationSize;
            };

            /**
             * The data returned when AttemptingConnect is invoked in the LifecycleEvents callback.
             * Currently empty, but may be used in the future for passing additional data.
             */
            struct AWS_CRT_CPP_API OnAttemptingConnectEventData
            {
                OnAttemptingConnectEventData() {}
            };

            /**
             * The data returned when OnConnectionFailure is invoked in the LifecycleEvents callback.
             */
            struct AWS_CRT_CPP_API OnConnectionFailureEventData
            {
                OnConnectionFailureEventData() : errorCode(AWS_ERROR_SUCCESS), connAckPacket(nullptr) {}

                int errorCode;
                std::shared_ptr<ConnAckPacket> connAckPacket;
            };

            /**
             * The data returned when OnConnectionSuccess is invoked in the LifecycleEvents callback.
             */
            struct AWS_CRT_CPP_API OnConnectionSuccessEventData
            {
                OnConnectionSuccessEventData() : connAckPacket(nullptr), negotiatedSettings(nullptr) {}

                std::shared_ptr<ConnAckPacket> connAckPacket;
                std::shared_ptr<NegotiatedSettings> negotiatedSettings;
            };

            /**
             * The data returned when OnDisconnect is invoked in the LifecycleEvents callback.
             */
            struct AWS_CRT_CPP_API OnDisconnectionEventData
            {
                OnDisconnectionEventData() : errorCode(AWS_ERROR_SUCCESS), disconnectPacket(nullptr) {}

                int errorCode;
                std::shared_ptr<DisconnectPacket> disconnectPacket;
            };

            /**
             * The data returned when OnStopped is invoked in the LifecycleEvents callback.
             * Currently empty, but may be used in the future for passing additional data.
             */
            struct AWS_CRT_CPP_API OnStoppedEventData
            {
                OnStoppedEventData() {}
            };

            /**
             * The data returned when a publish is made to a topic the MQTT5 client is subscribed to.
             */
            struct AWS_CRT_CPP_API PublishReceivedEventData
            {
                PublishReceivedEventData() : publishPacket(nullptr) {}
                std::shared_ptr<PublishPacket> publishPacket;
            };

            /**
             * Type signature of the callback invoked when connection succeed
             * Mandatory event fields: client, connack_data, settings
             */
            using OnConnectionSuccessHandler = std::function<void(const OnConnectionSuccessEventData &)>;

            /**
             * Type signature of the callback invoked when connection failed
             */
            using OnConnectionFailureHandler = std::function<void(const OnConnectionFailureEventData &)>;

            /**
             * Type signature of the callback invoked when the internal connection is shutdown
             */
            using OnDisconnectionHandler = std::function<void(const OnDisconnectionEventData &)>;

            /**
             * Type signature of the callback invoked when attempting connect to client
             * Mandatory event fields: client
             */
            using OnAttemptingConnectHandler = std::function<void(const OnAttemptingConnectEventData &)>;

            /**
             * Type signature of the callback invoked when client connection stopped
             * Mandatory event fields: client
             */
            using OnStoppedHandler = std::function<void(const OnStoppedEventData &)>;

            /**
             * Type signature of the callback invoked when a Publish Complete
             */
            using OnPublishCompletionHandler = std::function<void(int, std::shared_ptr<PublishResult>)>;

            /**
             * Type signature of the callback invoked when a Subscribe Complete
             */
            using OnSubscribeCompletionHandler = std::function<void(int, std::shared_ptr<SubAckPacket>)>;

            /**
             * Type signature of the callback invoked when a Unsubscribe Complete
             */
            using OnUnsubscribeCompletionHandler = std::function<void(int, std::shared_ptr<UnSubAckPacket>)>;

            /**
             * Type signature of the callback invoked when a PacketPublish message received (OnMessageHandler)
             */
            using OnPublishReceivedHandler = std::function<void(const PublishReceivedEventData &)>;

            /**
             * Callback for users to invoke upon completion of, presumably asynchronous, OnWebSocketHandshakeIntercept
             * callback's initiated process.
             */
            using OnWebSocketHandshakeInterceptComplete =
                std::function<void(const std::shared_ptr<Http::HttpRequest> &, int)>;

            /**
             * Invoked during websocket handshake to give users opportunity to transform an http request for purposes
             * such as signing/authorization etc... Returning from this function does not continue the websocket
             * handshake since some work flows may be asynchronous. To accommodate that, onComplete must be invoked upon
             * completion of the signing process.
             */
            using OnWebSocketHandshakeIntercept =
                std::function<void(std::shared_ptr<Http::HttpRequest>, const OnWebSocketHandshakeInterceptComplete &)>;

            /**
             * An MQTT5 client. This is a move-only type. Unless otherwise specified,
             * all function arguments need only to live through the duration of the
             * function call.
             */
            class AWS_CRT_CPP_API Mqtt5Client final : public std::enable_shared_from_this<Mqtt5Client>
            {
                friend class Mqtt::MqttConnection;

              public:
                /**
                 * Factory function for mqtt5 client
                 *
                 * @param options: Mqtt5 Client Options
                 * @param allocator allocator to use
                 * @return a new mqtt5 client
                 */
                static std::shared_ptr<Mqtt5Client> NewMqtt5Client(
                    const Mqtt5ClientOptions &options,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Get shared poitner of the Mqtt5Client. Mqtt5Client is inherited to enable_shared_from_this to help
                 * with memory safety.
                 *
                 * @return shared_ptr for the Mqtt5Client
                 */
                std::shared_ptr<Mqtt5Client> getptr() { return shared_from_this(); }

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Notifies the MQTT5 client that you want it to attempt to connect to the configured endpoint.
                 * The client will attempt to stay connected using the properties of the reconnect-related parameters
                 * from the client configuration.
                 *
                 * @return bool: true if operation succeed, otherwise false.
                 */
                bool Start() const noexcept;

                /**
                 * Notifies the MQTT5 client that you want it to transition to the stopped state, disconnecting any
                 * existing connection and stopping subsequent reconnect attempts.
                 *
                 * @return bool: true if operation succeed, otherwise false
                 */
                bool Stop() noexcept;

                /**
                 * Notifies the MQTT5 client that you want it to transition to the stopped state, disconnecting any
                 * existing connection and stopping subsequent reconnect attempts.
                 *
                 * @param disconnectPacket (optional) properties of a DISCONNECT packet to send as part of the shutdown
                 * process
                 *
                 * @return bool: true if operation succeed, otherwise false
                 */
                bool Stop(std::shared_ptr<DisconnectPacket> disconnectPacket) noexcept;

                /**
                 * Tells the client to attempt to send a PUBLISH packet
                 *
                 * @param publishPacket: packet PUBLISH to send to the server
                 * @param onPublishCompletionCallback: callback on publish complete, default to NULL
                 *
                 * @return true if the publish operation succeed otherwise false
                 */
                bool Publish(
                    std::shared_ptr<PublishPacket> publishPacket,
                    OnPublishCompletionHandler onPublishCompletionCallback = NULL) noexcept;

                /**
                 * Tells the client to attempt to subscribe to one or more topic filters.
                 *
                 * @param subscribePacket: SUBSCRIBE packet to send to the server
                 * @param onSubscribeCompletionCallback: callback on subscribe complete, default to NULL
                 *
                 * @return true if the subscription operation succeed otherwise false
                 */
                bool Subscribe(
                    std::shared_ptr<SubscribePacket> subscribePacket,
                    OnSubscribeCompletionHandler onSubscribeCompletionCallback = NULL) noexcept;

                /**
                 * Tells the client to attempt to unsubscribe to one or more topic filters.
                 *
                 * @param unsubscribePacket: UNSUBSCRIBE packet to send to the server
                 * @param onUnsubscribeCompletionCallback: callback on unsubscribe complete, default to NULL
                 *
                 * @return true if the unsubscription operation succeed otherwise false
                 */
                bool Unsubscribe(
                    std::shared_ptr<UnsubscribePacket> unsubscribePacket,
                    OnUnsubscribeCompletionHandler onUnsubscribeCompletionCallback = NULL) noexcept;

                /**
                 * Get the statistics about the current state of the client's queue of operations
                 *
                 * @return Mqtt5ClientOperationStatistics
                 */
                const Mqtt5ClientOperationStatistics &GetOperationStatistics() noexcept;

                virtual ~Mqtt5Client();

                struct aws_mqtt5_client *GetUnderlyingHandle() const noexcept;

              private:
                Mqtt5Client(const Mqtt5ClientOptions &options, Allocator *allocator = ApiAllocator()) noexcept;

                /* The client core to handle the user callbacks and c client termination */
                std::shared_ptr<Mqtt5ClientCore> m_client_core;

                Mqtt5ClientOperationStatistics m_operationStatistics;
            };

            /**
             * Configuration interface for mqtt5 clients
             */
            class AWS_CRT_CPP_API Mqtt5ClientOptions final
            {
                friend class Mqtt5ClientCore;
                friend class Mqtt5to3AdapterOptions;

              public:
                /**
                 * Default constructior of Mqtt5ClientOptions
                 */
                Mqtt5ClientOptions(Crt::Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Sets host to connect to.
                 *
                 * @param hostname endpoint to connect to
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithHostName(Crt::String hostname);

                /**
                 * Set port to connect to
                 *
                 * @param port port to connect to
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithPort(uint32_t port) noexcept;

                /**
                 * Set booststrap for mqtt5 client
                 *
                 * @param bootStrap bootstrap used for mqtt5 client. The default ClientBootstrap see
                 * Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap.
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithBootstrap(Io::ClientBootstrap *bootStrap) noexcept;

                /**
                 * Sets the aws socket options
                 *
                 * @param socketOptions  Io::SocketOptions used to setup socket
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithSocketOptions(Io::SocketOptions socketOptions) noexcept;

                /**
                 * Sets the tls connection options
                 *
                 * @param tslOptions  Io::TlsConnectionOptions
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithTlsConnectionOptions(const Io::TlsConnectionOptions &tslOptions) noexcept;

                /**
                 * Sets http proxy options.
                 *
                 * @param proxyOptions http proxy configuration for connection establishment
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithHttpProxyOptions(
                    const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept;

                /**
                 * Sets mqtt5 connection options
                 *
                 * @param connectPacket package connection options
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithConnectOptions(std::shared_ptr<ConnectPacket> connectPacket) noexcept;

                /**
                 * Sets session behavior. Overrides how the MQTT5 client should behave with respect to MQTT sessions.
                 *
                 * @param sessionBehavior
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithSessionBehavior(ClientSessionBehaviorType sessionBehavior) noexcept;

                /**
                 * Sets client extended validation and flow control, additional controls for client behavior with
                 * respect to operation validation and flow control; these checks go beyond the base MQTT5 spec to
                 * respect limits of specific MQTT brokers.
                 *
                 * @param clientExtendedValidationAndFlowControl
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientExtendedValidationAndFlowControl(
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
                Mqtt5ClientOptions &WithOfflineQueueBehavior(
                    ClientOperationQueueBehaviorType offlineQueueBehavior) noexcept;

                /**
                 * Sets ReconnectOptions. Reconnect options, includes retryJitterMode, min reconnect delay time and
                 * max reconnect delay time and reset reconnect delay time
                 *
                 * @param reconnectOptions
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithReconnectOptions(ReconnectOptions reconnectOptions) noexcept;

                /**
                 * Sets the topic aliasing behavior for the client.
                 *
                 * @param topicAliasingOptions topic aliasing behavior options to use
                 * @return this options object
                 */
                Mqtt5ClientOptions &WithTopicAliasingOptions(TopicAliasingOptions topicAliasingOptions) noexcept;

                /**
                 * Sets ping timeout (ms). Time interval to wait after sending a PINGREQ for a PINGRESP to arrive.
                 * If one does not arrive, the client will close the current connection.
                 *
                 * @param pingTimeoutMs
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithPingTimeoutMs(uint32_t pingTimeoutMs) noexcept;

                /**
                 * Sets Connack Timeout (ms). Time interval to wait after sending a CONNECT request for a CONNACK
                 * to arrive.  If one does not arrive, the connection will be shut down.
                 *
                 * @param connackTimeoutMs
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithConnackTimeoutMs(uint32_t connackTimeoutMs) noexcept;

                /**
                 * @deprecated The function is deprecated, please use `Mqtt5ClientOptions::WithAckTimeoutSec(uint32_t)`
                 *
                 * Sets Operation Timeout(Seconds). Time interval to wait for an ack after sending a QoS 1+ PUBLISH,
                 * SUBSCRIBE, or UNSUBSCRIBE before failing the operation.
                 *
                 * @param ackTimeoutSec
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithAckTimeoutSeconds(uint32_t ackTimeoutSec) noexcept;

                /**
                 * Sets Operation Timeout(Seconds). Time interval to wait for an ack after sending a QoS 1+ PUBLISH,
                 * SUBSCRIBE, or UNSUBSCRIBE before failing the operation.
                 *
                 * @param ackTimeoutSec
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithAckTimeoutSec(uint32_t ackTimeoutSec) noexcept;

                /**
                 * Sets callback for transform HTTP request.
                 * This callback allows a custom transformation of the HTTP request that acts as the websocket
                 * handshake. Websockets will be used if this is set to a valid transformation callback.  To use
                 * websockets but not perform a transformation, just set this as a trivial completion callback.  If
                 * undefined, the connection will be made with direct MQTT.
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithWebsocketHandshakeTransformCallback(
                    OnWebSocketHandshakeIntercept callback) noexcept;

                /**
                 * Sets callback trigged when client successfully establishes an MQTT connection
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientConnectionSuccessCallback(OnConnectionSuccessHandler callback) noexcept;

                /**
                 * Sets callback trigged when client fails to establish an MQTT connection
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientConnectionFailureCallback(OnConnectionFailureHandler callback) noexcept;

                /**
                 * Sets callback trigged when client's current MQTT connection is closed
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientDisconnectionCallback(OnDisconnectionHandler callback) noexcept;

                /**
                 * Sets callback trigged when client reaches the "Stopped" state
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientStoppedCallback(OnStoppedHandler callback) noexcept;

                /**
                 * Sets callback trigged when client begins an attempt to connect to the remote endpoint.
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithClientAttemptingConnectCallback(OnAttemptingConnectHandler callback) noexcept;

                /**
                 * Sets callback trigged when a PUBLISH packet is received by the client
                 *
                 * @param callback
                 *
                 * @return this option object
                 */
                Mqtt5ClientOptions &WithPublishReceivedCallback(OnPublishReceivedHandler callback) noexcept;

                /**
                 * Initializes the C aws_mqtt5_client_options from Mqtt5ClientOptions. For internal use
                 *
                 * @param raw_options - output parameter containing low level client options to be passed to the C
                 * interface
                 *
                 */
                bool initializeRawOptions(aws_mqtt5_client_options &raw_options) const noexcept;

                virtual ~Mqtt5ClientOptions();
                Mqtt5ClientOptions(const Mqtt5ClientOptions &) = delete;
                Mqtt5ClientOptions(Mqtt5ClientOptions &&) = delete;
                Mqtt5ClientOptions &operator=(const Mqtt5ClientOptions &) = delete;
                Mqtt5ClientOptions &operator=(Mqtt5ClientOptions &&) = delete;

              private:
                /**
                 * This callback allows a custom transformation of the HTTP request that acts as the websocket
                 * handshake. Websockets will be used if this is set to a valid transformation callback.  To use
                 * websockets but not perform a transformation, just set this as a trivial completion callback.  If
                 * undefined, the connection will be made with direct MQTT.
                 */
                OnWebSocketHandshakeIntercept websocketHandshakeTransform;

                /**
                 * Callback handler trigged when client successfully establishes an MQTT connection
                 */
                OnConnectionSuccessHandler onConnectionSuccess;

                /**
                 * Callback handler trigged when client fails to establish an MQTT connection
                 */
                OnConnectionFailureHandler onConnectionFailure;

                /**
                 * Callback handler trigged when client's current MQTT connection is closed
                 */
                OnDisconnectionHandler onDisconnection;

                /**
                 * Callback handler trigged when client reaches the "Stopped" state
                 *
                 * @param Mqtt5Client: The shared client
                 */
                OnStoppedHandler onStopped;

                /**
                 * Callback handler trigged when client begins an attempt to connect to the remote endpoint.
                 *
                 * @param Mqtt5Client: The shared client
                 */
                OnAttemptingConnectHandler onAttemptingConnect;

                /**
                 * Callback handler trigged when an MQTT PUBLISH packet is received by the client
                 *
                 * @param Mqtt5Client: The shared client
                 * @param PublishPacket: received Publish Packet
                 */
                OnPublishReceivedHandler onPublishReceived;

                /**
                 * Host name of the MQTT server to connect to.
                 */
                Crt::String m_hostName;

                /**
                 * Network port of the MQTT server to connect to.
                 */
                uint32_t m_port;

                /**
                 * Client bootstrap to use.  In almost all cases, this can be left undefined.
                 */
                Io::ClientBootstrap *m_bootstrap;

                /**
                 * Controls socket properties of the underlying MQTT connections made by the client.  Leave undefined to
                 * use defaults (no TCP keep alive, 10 second socket timeout).
                 */
                Crt::Io::SocketOptions m_socketOptions;

                /**
                 * TLS context for secure socket connections.
                 * If undefined, a plaintext connection will be used.
                 */
                Crt::Optional<Crt::Io::TlsConnectionOptions> m_tlsConnectionOptions;

                /**
                 * Configures (tunneling) HTTP proxy usage when establishing MQTT connections
                 */
                Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;

                /**
                 * All configurable options with respect to the CONNECT packet sent by the client, including the will.
                 * These connect properties will be used for every connection attempt made by the client.
                 */
                std::shared_ptr<ConnectPacket> m_connectOptions;

                /**
                 * Controls how the MQTT5 client should behave with respect to MQTT sessions.
                 */
                ClientSessionBehaviorType m_sessionBehavior;

                /**
                 * Additional controls for client behavior with respect to operation validation and flow control; these
                 * checks go beyond the base MQTT5 spec to respect limits of specific MQTT brokers.
                 */
                ClientExtendedValidationAndFlowControl m_extendedValidationAndFlowControlOptions;

                /**
                 * Controls how disconnects affect the queued and in-progress operations tracked by the client.  Also
                 * controls how new operations are handled while the client is not connected.  In particular, if the
                 * client is not connected, then any operation that would be failed on disconnect (according to these
                 * rules) will also be rejected.
                 */
                ClientOperationQueueBehaviorType m_offlineQueueBehavior;

                /**
                 * Reconnect options, includes retryJitterMode, min reconnect delay time and max reconnect delay time
                 */
                ReconnectOptions m_reconnectionOptions;

                /**
                 * Controls client topic aliasing behavior
                 */
                aws_mqtt5_client_topic_alias_options m_topicAliasingOptions;

                /**
                 * Time interval to wait after sending a PINGREQ for a PINGRESP to arrive.  If one does not arrive, the
                 * client will close the current connection.
                 */
                uint32_t m_pingTimeoutMs;

                /**
                 * Time interval to wait after sending a CONNECT request for a CONNACK to arrive.  If one does not
                 * arrive, the connection will be shut down.
                 */
                uint32_t m_connackTimeoutMs;

                /**
                 * Time interval to wait for an ack after sending a QoS 1+ PUBLISH, SUBSCRIBE, or UNSUBSCRIBE before
                 * failing the operation.
                 */
                uint32_t m_ackTimeoutSec;

                /* Underlying Parameters */
                Crt::Allocator *m_allocator;
                aws_http_proxy_options m_httpProxyOptionsStorage;
                aws_mqtt5_packet_connect_view m_packetConnectViewStorage;
            };

        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
