#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/Types.h>
#include <aws/crt/http/HttpConnection.h>
#include <aws/crt/io/SocketOptions.h>
#include <aws/crt/io/TlsOptions.h>
#include <aws/crt/mqtt/MqttTypes.h>

#include <aws/mqtt/client.h>
#include <aws/mqtt/v5/mqtt5_client.h>

#include <functional>
#include <memory>

namespace Aws
{
    namespace Crt
    {
        namespace Http
        {
            class HttpRequest;
        }

        namespace Mqtt5
        {
            class Mqtt5Client;
            class Mqtt5ClientCore;
        } // namespace Mqtt5

        namespace Mqtt
        {
            class MqttClient;
            class MqttConnectionCore;
            class MqttConnection;

            /**
             * The data returned when the connection closed callback is invoked in a connection.
             * Note: This class is currently empty, but this may contain data in the future.
             */
            struct OnConnectionClosedData
            {
            };

            /**
             * The data returned when the connection success callback is invoked in a connection.
             */
            struct OnConnectionSuccessData
            {
                /**
                 * The Connect return code received from the server.
                 */
                ReturnCode returnCode;

                /**
                 * Returns whether a session was present and resumed for this successful connection.
                 * Will be set to true if the connection resumed an already present MQTT connection session.
                 */
                bool sessionPresent;
            };

            /**
             * The data returned when the connection failure callback is invoked in a connection.
             */
            struct OnConnectionFailureData
            {
                /**
                 * The AWS CRT error code for the connection failure.
                 * Use Aws::Crt::ErrorDebugString to get a human readable string from the error code.
                 */
                int error;
            };

            /**
             * Invoked Upon Connection loss.
             */
            using OnConnectionInterruptedHandler = std::function<void(MqttConnection &connection, int error)>;

            /**
             * Invoked Upon Connection resumed.
             */
            using OnConnectionResumedHandler =
                std::function<void(MqttConnection &connection, ReturnCode connectCode, bool sessionPresent)>;

            /**
             * Invoked when a connack message is received, or an error occurred.
             */
            using OnConnectionCompletedHandler = std::function<
                void(MqttConnection &connection, int errorCode, ReturnCode returnCode, bool sessionPresent)>;

            /**
             * Invoked when a connection is disconnected and shutdown successfully.
             *
             * Note: Currently callbackData will always be nullptr, but this may change in the future to send additional
             * data.
             */
            using OnConnectionClosedHandler =
                std::function<void(MqttConnection &connection, OnConnectionClosedData *callbackData)>;

            /**
             * Invoked whenever the connection successfully connects.
             *
             * This callback is invoked for every successful connect and every successful reconnect.
             */
            using OnConnectionSuccessHandler =
                std::function<void(MqttConnection &connection, OnConnectionSuccessData *callbackData)>;

            /**
             * Invoked whenever the connection fails to connect.
             *
             * This callback is invoked for every failed connect and every failed reconnect.
             */
            using OnConnectionFailureHandler =
                std::function<void(MqttConnection &connection, OnConnectionFailureData *callbackData)>;

            /**
             * Invoked when a disconnect message has been sent.
             */
            using OnDisconnectHandler = std::function<void(MqttConnection &connection)>;

            /**
             * @deprecated Use OnMessageReceivedHandler
             */
            using OnPublishReceivedHandler =
                std::function<void(MqttConnection &connection, const String &topic, const ByteBuf &payload)>;

            /**
             * Callback for users to invoke upon completion of, presumably asynchronous, OnWebSocketHandshakeIntercept
             * callback's initiated process.
             */
            using OnWebSocketHandshakeInterceptComplete =
                std::function<void(const std::shared_ptr<Http::HttpRequest> &, int errorCode)>;

            /**
             * Invoked during websocket handshake to give users opportunity to transform an http request for purposes
             * such as signing/authorization etc... Returning from this function does not continue the websocket
             * handshake since some work flows may be asynchronous. To accommodate that, onComplete must be invoked upon
             * completion of the signing process.
             */
            using OnWebSocketHandshakeIntercept = std::function<
                void(std::shared_ptr<Http::HttpRequest> req, const OnWebSocketHandshakeInterceptComplete &onComplete)>;

            /**
             * Represents a persistent Mqtt Connection. The memory is owned by MqttClient or Mqtt5Client.
             *
             * To get a new instance of this class, use MqttClient::NewConnection or Mqtt5Client::NewConnection. Unless
             * specified all function arguments need only to live through the duration of the function call.
             *
             * @sa MqttClient::NewConnection
             * @sa Mqtt5Client::NewConnection
             */
            class AWS_CRT_CPP_API MqttConnection final : public std::enable_shared_from_this<MqttConnection>
            {
                friend class MqttClient;
                friend class Mqtt5::Mqtt5ClientCore;

              public:
                ~MqttConnection();
                MqttConnection(const MqttConnection &) = delete;
                MqttConnection(MqttConnection &&) = delete;
                MqttConnection &operator=(const MqttConnection &) = delete;
                MqttConnection &operator=(MqttConnection &&) = delete;

                /**
                 * Create a new MqttConnection object from the Mqtt5Client.
                 * @param mqtt5client The shared ptr of Mqtt5Client
                 *
                 * @return std::shared_ptr<Crt::Mqtt::MqttConnection>
                 */
                static std::shared_ptr<Crt::Mqtt::MqttConnection> NewConnectionFromMqtt5Client(
                    std::shared_ptr<Mqtt5::Mqtt5Client> mqtt5client) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Sets LastWill for the connection.
                 * @param topic topic the will message should be published to
                 * @param qos QOS the will message should be published with
                 * @param retain true if the will publish should be treated as a retained publish
                 * @param payload payload of the will message
                 * @return success/failure in setting the will
                 */
                bool SetWill(const char *topic, QOS qos, bool retain, const ByteBuf &payload) noexcept;

                /**
                 * Sets login credentials for the connection. The must get set before the Connect call
                 * if it is to be used.
                 * @param username user name to add to the MQTT CONNECT packet
                 * @param password password to add to the MQTT CONNECT packet
                 * @return success/failure
                 */
                bool SetLogin(const char *username, const char *password) noexcept;

                /**
                 * @deprecated Sets websocket proxy options. Replaced by SetHttpProxyOptions.
                 */
                bool SetWebsocketProxyOptions(const Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept;

                /**
                 * Sets http proxy options. In order to use an http proxy with mqtt either
                 *   (1) Websockets are used
                 *   (2) Mqtt-over-tls is used and the ALPN list of the tls context contains a tag that resolves to mqtt
                 *
                 * @param proxyOptions proxy configuration for making the mqtt connection
                 *
                 * @return success/failure
                 */
                bool SetHttpProxyOptions(const Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept;

                /**
                 * Customize time to wait between reconnect attempts.
                 * The time will start at min and multiply by 2 until max is reached.
                 * The time resets back to min after a successful connection.
                 * This function should only be called before Connect().
                 *
                 * @param min_seconds minimum time to wait before attempting a reconnect
                 * @param max_seconds maximum time to wait before attempting a reconnect
                 *
                 * @return success/failure
                 */
                bool SetReconnectTimeout(uint64_t min_seconds, uint64_t max_seconds) noexcept;

                /**
                 * Initiates the connection, OnConnectionCompleted will
                 * be invoked in an event-loop thread.
                 *
                 * @param clientId client identifier to use when establishing the mqtt connection
                 * @param cleanSession false to attempt to rejoin an existing session for the client id, true to skip
                 * and start with a new session
                 * @param keepAliveTimeSecs time interval to space mqtt pings apart by
                 * @param pingTimeoutMs timeout in milliseconds before the keep alive ping is considered to have failed
                 * @param protocolOperationTimeoutMs timeout in milliseconds to give up waiting for a response packet
                 * for an operation.  Necessary due to throttling properties on certain server implementations that do
                 * not return an ACK for throttled operations.
                 *
                 * @return true if the connection attempt was successfully started (implying a callback will be invoked
                 * with the eventual result), false if it could not be started (no callback will happen)
                 */
                bool Connect(
                    const char *clientId,
                    bool cleanSession,
                    uint16_t keepAliveTimeSecs = 0,
                    uint32_t pingTimeoutMs = 0,
                    uint32_t protocolOperationTimeoutMs = 0) noexcept;

                /**
                 * Initiates disconnect, OnDisconnectHandler will be invoked in an event-loop thread.
                 * @return success/failure in initiating disconnect
                 */
                bool Disconnect() noexcept;

                /// @private
                aws_mqtt_client_connection *GetUnderlyingConnection() const noexcept;

                /**
                 * Subscribes to topicFilter. OnMessageReceivedHandler will be invoked from an event-loop
                 * thread upon an incoming Publish message. OnSubAckHandler will be invoked
                 * upon receipt of a suback message.
                 *
                 * @param topicFilter topic filter to subscribe to
                 * @param qos maximum qos client is willing to receive matching messages on
                 * @param onMessage callback to invoke when a message is received based on matching this filter
                 * @param onSubAck callback to invoke with the server's response to the subscribe request
                 *
                 * @return packet id of the subscribe request, or 0 if the attempt failed synchronously
                 */
                uint16_t Subscribe(
                    const char *topicFilter,
                    QOS qos,
                    OnMessageReceivedHandler &&onMessage,
                    OnSubAckHandler &&onSubAck) noexcept;

                /**
                 * @deprecated Use alternate Subscribe()
                 */
                uint16_t Subscribe(
                    const char *topicFilter,
                    QOS qos,
                    OnPublishReceivedHandler &&onPublish,
                    OnSubAckHandler &&onSubAck) noexcept;

                /**
                 * Subscribes to multiple topicFilters. OnMessageReceivedHandler will be invoked from an event-loop
                 * thread upon an incoming Publish message. OnMultiSubAckHandler will be invoked
                 * upon receipt of a suback message.
                 *
                 * @param topicFilters list of pairs of topic filters and message callbacks to invoke on a matching
                 * publish
                 * @param qos maximum qos client is willing to receive matching messages on
                 * @param onOpComplete callback to invoke with the server's response to the subscribe request
                 *
                 * @return packet id of the subscribe request, or 0 if the attempt failed synchronously
                 */
                uint16_t Subscribe(
                    const Vector<std::pair<const char *, OnMessageReceivedHandler>> &topicFilters,
                    QOS qos,
                    OnMultiSubAckHandler &&onOpComplete) noexcept;

                /**
                 * @deprecated Use alternate Subscribe()
                 */
                uint16_t Subscribe(
                    const Vector<std::pair<const char *, OnPublishReceivedHandler>> &topicFilters,
                    QOS qos,
                    OnMultiSubAckHandler &&onOpComplete) noexcept;

                /**
                 * Installs a handler for all incoming publish messages, regardless of if Subscribe has been
                 * called on the topic.
                 *
                 * @param onMessage callback to invoke for all received messages
                 * @return success/failure
                 */
                bool SetOnMessageHandler(OnMessageReceivedHandler &&onMessage) noexcept;

                /**
                 * @deprecated Use alternate SetOnMessageHandler()
                 */
                bool SetOnMessageHandler(OnPublishReceivedHandler &&onPublish) noexcept;

                /**
                 * Unsubscribes from topicFilter. OnOperationCompleteHandler will be invoked upon receipt of
                 * an unsuback message.
                 *
                 * @param topicFilter topic filter to unsubscribe the session from
                 * @param onOpComplete callback to invoke on receipt of the server's UNSUBACK message
                 *
                 * @return packet id of the unsubscribe request, or 0 if the attempt failed synchronously
                 */
                uint16_t Unsubscribe(const char *topicFilter, OnOperationCompleteHandler &&onOpComplete) noexcept;

                /**
                 * Publishes to a topic.
                 *
                 * @param topic topic to publish to
                 * @param qos QOS to publish the message with
                 * @param retain should this message replace the current retained message of the topic?
                 * @param payload payload of the message
                 * @param onOpComplete completion callback to invoke when the operation is complete.  If QoS is 0, then
                 * the callback is invoked when the message is passed to the tls handler, otherwise it's invoked
                 * on receipt of the final response from the server.
                 *
                 * @return packet id of the publish request, or 0 if the attempt failed synchronously
                 */
                uint16_t Publish(
                    const char *topic,
                    QOS qos,
                    bool retain,
                    const ByteBuf &payload,
                    OnOperationCompleteHandler &&onOpComplete) noexcept;

                /**
                 * Get the statistics about the current state of the connection's queue of operations
                 *
                 * @return MqttConnectionOperationStatistics
                 */
                const MqttConnectionOperationStatistics &GetOperationStatistics() noexcept;

                /**
                 * A callback invoked every time the connections is interrupted.
                 */
                OnConnectionInterruptedHandler OnConnectionInterrupted;

                /**
                 * A callback invoked every time the connection is resumed.
                 */
                OnConnectionResumedHandler OnConnectionResumed;

                /**
                 * Invoked when a connack message is received, or an error occurred.
                 */
                OnConnectionCompletedHandler OnConnectionCompleted;

                /**
                 * A callback invoked on disconnect.
                 */
                OnDisconnectHandler OnDisconnect;

                /**
                 * Invoked during websocket handshake to give users opportunity to transform an http request for
                 * purposes such as signing/authorization etc... Returning from this function does not continue the
                 * websocket handshake since some work flows may be asynchronous. To accommodate that, onComplete must
                 * be invoked upon completion of the signing process.
                 */
                OnWebSocketHandshakeIntercept WebsocketInterceptor;

                /**
                 * Invoked when a connection is disconnected and shutdown successfully.
                 *
                 * @note Currently callbackData will always be nullptr, but this may change in the future to send
                 * additional data.
                 * @note From the user perspective, this callback is indistinguishable from OnDisconnect.
                 */
                OnConnectionClosedHandler OnConnectionClosed;

                /**
                 * Invoked whenever the connection successfully connects.
                 *
                 * This callback is invoked for every successful connect and every successful reconnect.
                 */
                OnConnectionSuccessHandler OnConnectionSuccess;

                /**
                 * Invoked whenever the connection fails to connect.
                 *
                 * This callback is invoked for every failed connect and every failed reconnect.
                 */
                OnConnectionFailureHandler OnConnectionFailure;

              private:
                /**
                 * Constructor.
                 *
                 * Make private to restrict ability to create MqttConnections objects to certain classes.
                 */
                MqttConnection() = default;

                /**
                 * @internal
                 * Factory method for creating MqttConnection.
                 *
                 * @param client MQTT3 client.
                 * @param options Options required for MqttConnection creation.
                 * @return New instance of MqttConnection.
                 */
                static std::shared_ptr<MqttConnection> s_CreateMqttConnection(
                    aws_mqtt_client *client,
                    MqttConnectionOptions options) noexcept;

                /**
                 * @internal
                 * Factory method for creating MqttConnection.
                 *
                 * @param mqtt5Client MQTT5 client.
                 * @param options Options required for MqttConnection creation.
                 * @return New instance of MqttConnection.
                 */
                static std::shared_ptr<MqttConnection> s_CreateMqttConnection(
                    aws_mqtt5_client *mqtt5Client,
                    MqttConnectionOptions options) noexcept;
                /**
                 * @internal
                 * Internal handler for the underlying connection.
                 */
                std::shared_ptr<MqttConnectionCore> m_connectionCore;
            };
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
