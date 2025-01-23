/*! \cond DOXYGEN_PRIVATE
** Hide API from this file in doxygen. Set DOXYGEN_PRIVATE in doxygen
** config to enable this file for doxygen.
*/
#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Types.h>

#include <mutex>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            /**
             * The Mqtt5ClientCore is an internal class for Mqtt5Client. The class is used to handle communication
             * between Mqtt5Client and underlying c mqtt5 client. This class should only be used internally by
             * Mqtt5Client. This is a move-only type. Unless otherwise specified, all function arguments need only to
             * live through the duration of the function call.
             */
            class AWS_CRT_CPP_API Mqtt5ClientCore final : public std::enable_shared_from_this<Mqtt5ClientCore>
            {
                friend class Mqtt5Client;
                friend class Mqtt::MqttConnection;

              public:
                /**
                 * Factory function for mqtt5 client core
                 *
                 * @param options: Mqtt5 Client Options
                 * @param allocator allocator to use
                 * @return a new mqtt5 client
                 */
                static std::shared_ptr<Mqtt5ClientCore> NewMqtt5ClientCore(
                    const Mqtt5ClientOptions &options,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Get shared poitner of the Mqtt5ClientCore. Mqtt5ClientCore is inherited to enable_shared_from_this to
                 * help with memory safety.
                 *
                 * @return shared_ptr for the Mqtt5ClientCore
                 */
                std::shared_ptr<Mqtt5ClientCore> getptr() { return shared_from_this(); }

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Tells the client to attempt to send a PUBLISH packet
                 *
                 * @param publishOptions: packet PUBLISH to send to the server
                 * @param onPublishCompletionCallback: callback on publish complete, default to NULL
                 *
                 * @return true if the publish operation succeed otherwise false
                 */
                bool Publish(
                    std::shared_ptr<PublishPacket> publishOptions,
                    OnPublishCompletionHandler onPublishCompletionCallback = NULL) noexcept;

                /**
                 * Tells the client to attempt to subscribe to one or more topic filters.
                 *
                 * @param subscribeOptions: SUBSCRIBE packet to send to the server
                 * @param onSubscribeCompletionCallback: callback on subscribe complete, default to NULL
                 *
                 * @return true if the subscription operation succeed otherwise false
                 */
                bool Subscribe(
                    std::shared_ptr<SubscribePacket> subscribeOptions,
                    OnSubscribeCompletionHandler onSubscribeCompletionCallback = NULL) noexcept;

                /**
                 * Tells the client to attempt to unsubscribe to one or more topic filters.
                 *
                 * @param unsubscribeOptions: UNSUBSCRIBE packet to send to the server
                 * @param onUnsubscribeCompletionCallback: callback on unsubscribe complete, default to NULL
                 *
                 * @return true if the unsubscription operation succeed otherwise false
                 */
                bool Unsubscribe(
                    std::shared_ptr<UnsubscribePacket> unsubscribeOptions,
                    OnUnsubscribeCompletionHandler onUnsubscribeCompletionCallback = NULL) noexcept;

                /**
                 * Tells the Mqtt5ClientCore to release the native client and clean up unhandled the resources
                 * and operations before destroying it. You MUST only call this function when you
                 * want to destroy the Mqtt5ClientCore.
                 *
                 * IMPORTANT: After the function is invoked, the Mqtt5ClientCore will become invalid. DO
                 * NOT call the function unless you plan to destroy the client.
                 *
                 */
                void Close() noexcept;

                virtual ~Mqtt5ClientCore();

                struct aws_mqtt5_client *GetUnderlyingHandle() const noexcept { return m_client; }

              private:
                Mqtt5ClientCore(const Mqtt5ClientOptions &options, Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Create a new connection object over plain text from the Mqtt5Client. The client must outlive
                 * all of its connection instances. The Mqtt5 Options will be overwritten by the options passed in here.
                 *
                 * @param options the options from Mqtt5Client used to support the MqttConnection
                 *
                 * @return std::shared_ptr<Crt::Mqtt::MqttConnection>
                 */
                std::shared_ptr<Crt::Mqtt::MqttConnection> NewConnection(
                    const Mqtt5::Mqtt5to3AdapterOptions *options) noexcept;

                /* Static Callbacks */
                static void s_publishCompletionCallback(
                    enum aws_mqtt5_packet_type packet_type,
                    const void *packet,
                    int error_code,
                    void *complete_ctx);

                static void s_subscribeCompletionCallback(
                    const struct aws_mqtt5_packet_suback_view *puback,
                    int error_code,
                    void *complete_ctx);

                static void s_unsubscribeCompletionCallback(
                    const struct aws_mqtt5_packet_unsuback_view *puback,
                    int error_code,
                    void *complete_ctx);

                static void s_lifeCycleEventCallback(const aws_mqtt5_client_lifecycle_event *event);

                static void s_publishReceivedCallback(const aws_mqtt5_packet_publish_view *publish, void *user_data);

                static void s_onWebsocketHandshake(
                    aws_http_message *rawRequest,
                    void *user_data,
                    aws_mqtt5_transform_websocket_handshake_complete_fn *complete_fn,
                    void *complete_ctx);

                static void s_clientTerminationCompletion(void *complete_ctx);

                /* The handler is set by clientoptions */
                OnWebSocketHandshakeIntercept websocketInterceptor;
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
                 */
                OnStoppedHandler onStopped;

                /**
                 * Callback handler trigged when client begins an attempt to connect to the remote endpoint.
                 */
                OnAttemptingConnectHandler onAttemptingConnect;

                /**
                 * Callback handler trigged when an MQTT PUBLISH packet is received by the client
                 */
                OnPublishReceivedHandler onPublishReceived;

                /**
                 * The self reference is used to keep the Mqtt5ClientCore alive until the underlying
                 * m_client get terminated.
                 */
                std::shared_ptr<Mqtt5ClientCore> m_selfReference;

                /*
                 * The Mqtt5to3 Adapter Options. Used to create a mqtt311 connection from mqtt5 client
                 */
                ScopedResource<Mqtt5to3AdapterOptions> m_mqtt5to3AdapterOptions;

                /*
                 * The callback flag used to indicate if it is safe to invoke the callbacks
                 */
                enum CallbackFlag
                {
                    INVOKE,
                    IGNORE
                } m_callbackFlag;

                /*
                 * Lock for the callbacks. This is used to protect the callback flag and callbacks.
                 */
                std::recursive_mutex m_callback_lock;

                aws_mqtt5_client *m_client;
                Allocator *m_allocator;
            };

            /**
             * The extra options required to build MqttConnection from Mqtt5Client
             */
            class Mqtt5to3AdapterOptions
            {
                friend class Mqtt5ClientOptions;
                friend class Mqtt5ClientCore;
                friend class Mqtt::MqttConnection;

              public:
                /* Default constructor */
                Mqtt5to3AdapterOptions();
                /*
                 * Allocate and create a new Mqtt5to3AdapterOptions. This function is internally used by Mqtt5Client to
                 * support the Mqtt5to3Adapter.
                 *
                 * @return Mqtt5to3AdapterOptions
                 */
                static ScopedResource<Mqtt5to3AdapterOptions> NewMqtt5to3AdapterOptions(
                    const Mqtt5ClientOptions &options) noexcept;

              private:
                Mqtt::MqttConnectionOptions m_mqtt3Options;

                /* Reserve to store memory for m_mqtt3options.hostname */
                String m_hostname;

                /*
                 * The transform function invoked during websocket handshake.
                 */
                Crt::Mqtt::OnWebSocketHandshakeIntercept m_webSocketInterceptor;

                /* Store the user intercept handshake function */
                OnWebSocketHandshakeIntercept m_websocketHandshakeTransform;

                /**
                 * Configures (tunneling) HTTP proxy usage when establishing MQTT connections
                 */
                Crt::Optional<Crt::Http::HttpClientConnectionProxyOptions> m_proxyOptions;
            };

        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
/*! \endcond */
