/*! \cond DOXYGEN_PRIVATE
** Hide API from this file in doxygen. Set DOXYGEN_PRIVATE in doxygen
** config to enable this file for doxygen.
*/
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
        namespace Mqtt
        {
            class MqttConnection;

            /**
             * @internal
             * The MqttConnectionCore is an internal class for MqttConnection. The class is used to handle communication
             * between MqttConnection and underlying C MQTT client. This class should only be used internally by
             * MqttConnection.
             * @note Unless otherwise specified, all function arguments need only to live through the duration of the
             * function call.
             */
            class MqttConnectionCore final : public std::enable_shared_from_this<MqttConnectionCore>
            {
                friend MqttConnection;

              public:
                ~MqttConnectionCore();
                MqttConnectionCore(const MqttConnectionCore &) = delete;
                MqttConnectionCore(MqttConnectionCore &&) = delete;
                MqttConnectionCore &operator=(const MqttConnectionCore &) = delete;
                MqttConnectionCore &operator=(MqttConnectionCore &&) = delete;

                /**
                 * @internal
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @internal
                 * Tells the MqttConnectionCore to release the native client and clean up unhandled resources and
                 * operations before destroying it.
                 *
                 * @attention After the function is invoked, the MqttConnectionCore object becomes invalid.
                 */
                void Destroy();

                /**
                 * @internal
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * @internal
                 * Sets LastWill for the connection.
                 * @param topic topic the will message should be published to
                 * @param qos QOS the will message should be published with
                 * @param retain true if the will publish should be treated as a retained publish
                 * @param payload payload of the will message
                 * @return success/failure in setting the will
                 */
                bool SetWill(const char *topic, QOS qos, bool retain, const ByteBuf &payload) noexcept;

                /**
                 * @internal
                 * Sets login credentials for the connection. The must get set before the Connect call
                 * if it is to be used.
                 * @param username user name to add to the MQTT CONNECT packet
                 * @param password password to add to the MQTT CONNECT packet
                 * @return success/failure
                 */
                bool SetLogin(const char *username, const char *password) noexcept;

                /**
                 * @internal
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
                 * @internal
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
                 * @internal
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
                 * @param setWebSocketInterceptor Determines if websocket interceptor callback should be setup.
                 *
                 * @return true if the connection attempt was successfully started (implying a callback will be invoked
                 * with the eventual result), false if it could not be started (no callback will happen)
                 */
                bool Connect(
                    const char *clientId,
                    bool cleanSession,
                    uint16_t keepAliveTimeSecs,
                    uint32_t pingTimeoutMs,
                    uint32_t protocolOperationTimeoutMs,
                    bool setWebSocketInterceptor) noexcept;

                /**
                 * @internal
                 * Initiates disconnect, OnDisconnectHandler will be invoked in an event-loop thread.
                 * @return success/failure in initiating disconnect
                 */
                bool Disconnect() noexcept;

                /// @private
                aws_mqtt_client_connection *GetUnderlyingConnection() const noexcept;

                /**
                 * @internal
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
                 * @internal
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
                 * @internal
                 * Installs a handler for all incoming publish messages, regardless of if Subscribe has been
                 * called on the topic.
                 *
                 * @param onMessage callback to invoke for all received messages
                 * @return success/failure
                 */
                bool SetOnMessageHandler(OnMessageReceivedHandler &&onMessage) noexcept;

                /**
                 * @internal
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
                 * @internal
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
                 * @internal
                 * Get the statistics about the current state of the connection's queue of operations
                 *
                 * @return MqttConnectionOperationStatistics
                 */
                const MqttConnectionOperationStatistics &GetOperationStatistics() noexcept;

              private:
                /**
                 * @internal
                 * Factory method for instantiation of MqttConnectCore.
                 * @param client MQTT3 client.
                 * @param connection MqttConnection object, it's used for user callbacks.
                 * @param options Options required to create connection.
                 */
                static std::shared_ptr<MqttConnectionCore> s_createMqttConnectionCore(
                    aws_mqtt_client *client,
                    std::shared_ptr<MqttConnection> connection,
                    MqttConnectionOptions options) noexcept;

                /**
                 * @internal
                 * Factory method for instantiation of MqttConnectCore.
                 * @param mqtt5Client MQTT5 client.
                 * @param connection MqttConnection object, it's used for user callbacks.
                 * @param options Options required to create connection.
                 */
                static std::shared_ptr<MqttConnectionCore> s_createMqttConnectionCore(
                    aws_mqtt5_client *mqtt5Client,
                    std::shared_ptr<MqttConnection> connection,
                    MqttConnectionOptions options) noexcept;

                static void s_onConnectionTermination(void *userData);

                static void s_onConnectionInterrupted(aws_mqtt_client_connection *, int errorCode, void *userData);
                static void s_onConnectionCompleted(
                    aws_mqtt_client_connection *,
                    int errorCode,
                    enum aws_mqtt_connect_return_code returnCode,
                    bool sessionPresent,
                    void *userData);

                static void s_onConnectionSuccess(
                    aws_mqtt_client_connection *,
                    ReturnCode returnCode,
                    bool sessionPresent,
                    void *userData);

                static void s_onConnectionFailure(aws_mqtt_client_connection *, int errorCode, void *userData);

                static void s_onConnectionResumed(
                    aws_mqtt_client_connection *,
                    ReturnCode returnCode,
                    bool sessionPresent,
                    void *userData);

                static void s_onConnectionClosed(
                    aws_mqtt_client_connection *,
                    on_connection_closed_data *data,
                    void *userData);

                static void s_onDisconnect(aws_mqtt_client_connection *connection, void *userData);
                static void s_onPublish(
                    aws_mqtt_client_connection *connection,
                    const aws_byte_cursor *topic,
                    const aws_byte_cursor *payload,
                    bool dup,
                    enum aws_mqtt_qos qos,
                    bool retain,
                    void *userData);

                static void s_onSubAck(
                    aws_mqtt_client_connection *connection,
                    uint16_t packetId,
                    const struct aws_byte_cursor *topic,
                    enum aws_mqtt_qos qos,
                    int errorCode,
                    void *userdata);
                static void s_onMultiSubAck(
                    aws_mqtt_client_connection *connection,
                    uint16_t packetId,
                    const struct aws_array_list *topicSubacks,
                    int errorCode,
                    void *userdata);
                static void s_onOpComplete(
                    aws_mqtt_client_connection *connection,
                    uint16_t packetId,
                    int errorCode,
                    void *userdata);

                static void s_onWebsocketHandshake(
                    struct aws_http_message *request,
                    void *userData,
                    aws_mqtt_transform_websocket_handshake_complete_fn *completeFn,
                    void *completeCtx);

                /**
                 * @internal
                 * Constructor.
                 *
                 * @note Do not create MqttConnectionCore directly, MqttConnectionCore::s_createMqttConnectionCore
                 * should be used for instantiation.
                 *
                 * @param client MQTT3 client.
                 * @param mqtt5Client MQTT5 client.
                 * @param connection MqttConnection object, it's used for user callbacks.
                 * @param options Options required to create connection.
                 *
                 */
                MqttConnectionCore(
                    aws_mqtt_client *client,
                    aws_mqtt5_client *mqtt5Client,
                    std::shared_ptr<MqttConnection> connection,
                    MqttConnectionOptions options) noexcept;

                void createUnderlyingConnection(aws_mqtt_client *mqttClient);
                void createUnderlyingConnection(aws_mqtt5_client *mqtt5Client);
                void connectionInit();

                /**
                 * @internal
                 * Try to obtain in a thread-safe manner a shared_ptr of the MqttConnection object.
                 * @return A std::shared_ptr of the MqttConnection object if it's still alive, empty std::shared_ptr
                 * otherwise.
                 */
                std::shared_ptr<MqttConnection> obtainConnectionInstance();

                aws_mqtt_client_connection *m_underlyingConnection;
                String m_hostName;
                uint32_t m_port;
                Crt::Io::TlsContext m_tlsContext;
                Io::TlsConnectionOptions m_tlsOptions;
                Io::SocketOptions m_socketOptions;
                Crt::Optional<Http::HttpClientConnectionProxyOptions> m_proxyOptions;
                void *m_onAnyCbData;
                bool m_useTls;
                bool m_useWebsocket;
                MqttConnectionOperationStatistics m_operationStatistics;
                Allocator *m_allocator;

                /**
                 * @internal
                 * The MqttConnection object which created this MqttConnectionCore object.
                 *
                 * We have to store this object here to be able to pass it to the user callbacks.
                 */
                std::weak_ptr<MqttConnection> m_connection;

                /**
                 * @internal
                 * The self reference is used to keep the MqttConnectionCore alive until the underlying connections is
                 * destroyed.
                 */
                std::shared_ptr<MqttConnectionCore> m_self;
            };
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
/*! \endcond */
