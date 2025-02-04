/*! \cond DOXYGEN_PRIVATE
** Hide API from this file in doxygen. Set DOXYGEN_PRIVATE in doxygen
** config to enable this file for doxygen.
*/
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/mqtt/private/MqttConnectionCore.h>

#include <aws/crt/Api.h>
#include <aws/crt/http/HttpRequestResponse.h>

#define AWS_MQTT_MAX_TOPIC_LENGTH 65535

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt
        {
            /**
             * @internal
             * Auxiliary struct providing access to a user callback and a connection object in callbacks triggered by
             * publish events from the active subscriptions.
             */
            struct PubCallbackData
            {
                MqttConnectionCore *connectionCore = nullptr;
                OnMessageReceivedHandler onMessageReceived;
                Allocator *allocator = nullptr;
            };

            MqttConnectionCore::MqttConnectionCore(
                aws_mqtt_client *client,
                aws_mqtt5_client *mqtt5Client,
                std::shared_ptr<MqttConnection> connection,
                MqttConnectionOptions options) noexcept
                : m_underlyingConnection(nullptr), m_hostName(options.hostName), m_port(options.port),
                  m_tlsContext(std::move(options.tlsContext)), m_tlsOptions(std::move(options.tlsConnectionOptions)),
                  m_socketOptions(std::move(options.socketOptions)), m_onAnyCbData(nullptr), m_useTls(options.useTls),
                  m_useWebsocket(options.useWebsocket), m_allocator(options.allocator),
                  m_connection(std::move(connection))
            {
                if (client != nullptr)
                {
                    createUnderlyingConnection(client);
                }
                else if (mqtt5Client != nullptr)
                {
                    createUnderlyingConnection(mqtt5Client);
                }
                connectionInit();
            }

            MqttConnectionCore::~MqttConnectionCore()
            {
                if (*this && m_onAnyCbData != nullptr)
                {
                    auto *pubCallbackData = reinterpret_cast<PubCallbackData *>(m_onAnyCbData);
                    Crt::Delete(pubCallbackData, pubCallbackData->allocator);
                }
            }

            std::shared_ptr<MqttConnectionCore> MqttConnectionCore::s_createMqttConnectionCore(
                aws_mqtt_client *client,
                std::shared_ptr<MqttConnection> connection,
                MqttConnectionOptions options) noexcept
            {
                auto *allocator = options.allocator;
                auto *toSeat =
                    reinterpret_cast<MqttConnectionCore *>(aws_mem_acquire(allocator, sizeof(MqttConnectionCore)));
                if (toSeat == nullptr)
                {
                    return {};
                }

                toSeat = new (toSeat) MqttConnectionCore(client, nullptr, std::move(connection), std::move(options));
                if (!*toSeat)
                {
                    Crt::Delete(toSeat, allocator);
                    return nullptr;
                }

                auto connectionCore = std::shared_ptr<MqttConnectionCore>(
                    toSeat, [allocator](MqttConnectionCore *ptr) { Crt::Delete(ptr, allocator); });
                connectionCore->m_self = connectionCore;
                return connectionCore;
            }

            std::shared_ptr<MqttConnectionCore> MqttConnectionCore::s_createMqttConnectionCore(
                aws_mqtt5_client *mqtt5Client,
                std::shared_ptr<MqttConnection> connection,
                MqttConnectionOptions options) noexcept
            {
                auto *allocator = options.allocator;
                auto *toSeat =
                    reinterpret_cast<MqttConnectionCore *>(aws_mem_acquire(allocator, sizeof(MqttConnectionCore)));
                if (toSeat == nullptr)
                {
                    return {};
                }

                toSeat =
                    new (toSeat) MqttConnectionCore(nullptr, mqtt5Client, std::move(connection), std::move(options));
                if (!*toSeat)
                {
                    Crt::Delete(toSeat, allocator);
                    return nullptr;
                }

                auto connectionCore = std::shared_ptr<MqttConnectionCore>(
                    toSeat, [allocator](MqttConnectionCore *ptr) { Crt::Delete(ptr, allocator); });
                connectionCore->m_self = connectionCore;
                return connectionCore;
            }

            void MqttConnectionCore::s_onConnectionTermination(void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                // The underlying connection is destroyed, so no one can access the Core object, so it's safe to
                // initiate self-destruction by releasing owning pointer to itself.
                connectionCore->m_self.reset();
            }

            void MqttConnectionCore::s_onConnectionInterrupted(
                aws_mqtt_client_connection * /*connection*/,
                int errorCode,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionInterrupted)
                {
                    connection->OnConnectionInterrupted(*connection, errorCode);
                }
            }

            void MqttConnectionCore::s_onConnectionResumed(
                aws_mqtt_client_connection * /*connection*/,
                ReturnCode returnCode,
                bool sessionPresent,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionResumed)
                {
                    connection->OnConnectionResumed(*connection, returnCode, sessionPresent);
                }
                if (connection->OnConnectionSuccess)
                {
                    OnConnectionSuccessData callbackData;
                    callbackData.returnCode = returnCode;
                    callbackData.sessionPresent = sessionPresent;
                    connection->OnConnectionSuccess(*connection, &callbackData);
                }
            }

            void MqttConnectionCore::s_onConnectionClosed(
                aws_mqtt_client_connection * /*underlying_connection*/,
                on_connection_closed_data *data,
                void *userData)
            {
                (void)data;

                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionClosed)
                {
                    connection->OnConnectionClosed(*connection, nullptr);
                }
            }

            void MqttConnectionCore::s_onConnectionCompleted(
                aws_mqtt_client_connection * /*underlying_connection*/,
                int errorCode,
                enum aws_mqtt_connect_return_code returnCode,
                bool sessionPresent,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionCompleted)
                {
                    connection->OnConnectionCompleted(*connection, errorCode, returnCode, sessionPresent);
                }
            }

            void MqttConnectionCore::s_onConnectionSuccess(
                aws_mqtt_client_connection * /*underlying_connection*/,
                ReturnCode returnCode,
                bool sessionPresent,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionSuccess)
                {
                    OnConnectionSuccessData callbackData;
                    callbackData.returnCode = returnCode;
                    callbackData.sessionPresent = sessionPresent;
                    connection->OnConnectionSuccess(*connection, &callbackData);
                }
            }

            void MqttConnectionCore::s_onConnectionFailure(
                aws_mqtt_client_connection * /*underlying_connection*/,
                int errorCode,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnConnectionFailure)
                {
                    OnConnectionFailureData callbackData;
                    callbackData.error = errorCode;
                    connection->OnConnectionFailure(*connection, &callbackData);
                }
            }

            void MqttConnectionCore::s_onDisconnect(
                aws_mqtt_client_connection * /*underlying_connection*/,
                void *userData)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                if (connection->OnDisconnect)
                {
                    connection->OnDisconnect(*connection);
                }
            }

            static void s_cleanUpOnPublishData(void *userData)
            {
                auto *callbackData = reinterpret_cast<PubCallbackData *>(userData);
                Crt::Delete(callbackData, callbackData->allocator);
            }

            void MqttConnectionCore::s_onPublish(
                aws_mqtt_client_connection * /*connection*/,
                const aws_byte_cursor *topic,
                const aws_byte_cursor *payload,
                bool dup,
                enum aws_mqtt_qos qos,
                bool retain,
                void *userData)
            {
                auto *callbackData = reinterpret_cast<PubCallbackData *>(userData);
                if (!callbackData->onMessageReceived)
                {
                    return;
                }

                auto *connectionCore = callbackData->connectionCore;
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                String topicStr(reinterpret_cast<char *>(topic->ptr), topic->len);
                ByteBuf payloadBuf = aws_byte_buf_from_array(payload->ptr, payload->len);
                callbackData->onMessageReceived(*connection, topicStr, payloadBuf, dup, qos, retain);
            }

            struct OpCompleteCallbackData
            {
                MqttConnectionCore *connectionCore = nullptr;
                OnOperationCompleteHandler onOperationComplete;
                Allocator *allocator = nullptr;
            };

            void MqttConnectionCore::s_onOpComplete(
                aws_mqtt_client_connection * /*connection*/,
                uint16_t packetId,
                int errorCode,
                void *userData)
            {
                auto *callbackData = reinterpret_cast<OpCompleteCallbackData *>(userData);

                if (callbackData->onOperationComplete)
                {
                    auto connection = callbackData->connectionCore->obtainConnectionInstance();
                    if (connection)
                    {
                        callbackData->onOperationComplete(*connection, packetId, errorCode);
                    }
                }

                // Clean up previously allocated resources.
                Crt::Delete(callbackData, callbackData->allocator);
            }

            struct SubAckCallbackData
            {
                MqttConnectionCore *connectionCore = nullptr;
                OnSubAckHandler onSubAck;
                const char *topic = nullptr;
                Allocator *allocator = nullptr;
            };

            void MqttConnectionCore::s_onSubAck(
                aws_mqtt_client_connection * /*connection*/,
                uint16_t packetId,
                const struct aws_byte_cursor *topic,
                enum aws_mqtt_qos qos,
                int errorCode,
                void *userData)
            {
                auto *callbackData = reinterpret_cast<SubAckCallbackData *>(userData);

                if (callbackData->onSubAck)
                {
                    auto connection = callbackData->connectionCore->obtainConnectionInstance();
                    if (connection)
                    {
                        String topicStr(reinterpret_cast<char *>(topic->ptr), topic->len);
                        callbackData->onSubAck(*connection, packetId, topicStr, qos, errorCode);
                    }
                }

                // Clean up previously allocated resources.
                if (callbackData->topic != nullptr)
                {
                    aws_mem_release(
                        callbackData->allocator, reinterpret_cast<void *>(const_cast<char *>(callbackData->topic)));
                }
                Crt::Delete(callbackData, callbackData->allocator);
            }

            struct MultiSubAckCallbackData
            {
                MqttConnectionCore *connectionCore = nullptr;
                OnMultiSubAckHandler onSubAck;
                const char *topic = nullptr;
                Allocator *allocator = nullptr;
            };

            void MqttConnectionCore::s_onMultiSubAck(
                aws_mqtt_client_connection * /*connection*/,
                uint16_t packetId,
                const struct aws_array_list *topicSubacks,
                int errorCode,
                void *userData)
            {
                auto *callbackData = reinterpret_cast<MultiSubAckCallbackData *>(userData);

                if (callbackData->onSubAck)
                {
                    auto connection = callbackData->connectionCore->obtainConnectionInstance();
                    if (connection)
                    {
                        size_t length = aws_array_list_length(topicSubacks);
                        Vector<String> topics;
                        topics.reserve(length);
                        QOS qos = AWS_MQTT_QOS_AT_MOST_ONCE;
                        for (size_t i = 0; i < length; ++i)
                        {
                            aws_mqtt_topic_subscription *subscription = nullptr;
                            aws_array_list_get_at(topicSubacks, &subscription, i);
                            topics.emplace_back(
                                reinterpret_cast<char *>(subscription->topic.ptr), subscription->topic.len);
                            // TODO Is only the last one needed?
                            qos = subscription->qos;
                        }

                        callbackData->onSubAck(*connection, packetId, topics, qos, errorCode);
                    }
                }

                // Clean up previously allocated resources.
                if (callbackData->topic != nullptr)
                {
                    aws_mem_release(
                        callbackData->allocator, reinterpret_cast<void *>(const_cast<char *>(callbackData->topic)));
                }
                Crt::Delete(callbackData, callbackData->allocator);
            }

            void MqttConnectionCore::createUnderlyingConnection(aws_mqtt_client *client)
            {
                m_underlyingConnection = aws_mqtt_client_connection_new(client);
            }

            void MqttConnectionCore::createUnderlyingConnection(aws_mqtt5_client *mqtt5Client)
            {
                m_underlyingConnection = aws_mqtt_client_connection_new_from_mqtt5_client(mqtt5Client);
            }

            void MqttConnectionCore::connectionInit()
            {
                if (m_underlyingConnection != nullptr)
                {
                    aws_mqtt_client_connection_set_connection_result_handlers(
                        m_underlyingConnection,
                        MqttConnectionCore::s_onConnectionSuccess,
                        this,
                        MqttConnectionCore::s_onConnectionFailure,
                        this);

                    aws_mqtt_client_connection_set_connection_interruption_handlers(
                        m_underlyingConnection,
                        MqttConnectionCore::s_onConnectionInterrupted,
                        this,
                        MqttConnectionCore::s_onConnectionResumed,
                        this);

                    aws_mqtt_client_connection_set_connection_closed_handler(
                        m_underlyingConnection, MqttConnectionCore::s_onConnectionClosed, this);

                    aws_mqtt_client_connection_set_connection_termination_handler(
                        m_underlyingConnection, MqttConnectionCore::s_onConnectionTermination, this);
                }
                else
                {
                    AWS_LOGF_DEBUG(AWS_LS_MQTT_CLIENT, "Failed to initialize Mqtt Connection");
                }
            }

            std::shared_ptr<MqttConnection> MqttConnectionCore::obtainConnectionInstance()
            {
                // std::weak_ptr::lock will return std::shared_ptr of the managed object ONLY IF there is at least one
                // other alive instance.
                // If the last alive shared_ptr (on the user side) is being destroyed in parallel with this code, they
                // will try to atomically increment/decrement the shared ref counter (equal to 1). So, the two following
                // scenarios are possible.
                // 1. std::weak_ptr::lock increments the ref counter first.
                //   - std::weak_ptr::lock atomically increments the ref counter to 2.
                //   - The user shared_ptr's destructor atomically decrements the ref counter to 1.
                //   - When the shared_ptr object obtained here is destroyed, the ref counter decremented to 0.
                //   - The MqttConnection object destructor called.
                // 2. User's shared_ptr decrements the ref counter first.
                //   - The user shared_ptr's destructor atomically decrements the ref counter to 0.
                //   - std::weak_ptr::lock tries to increment the ref counter, but fails.
                //   - std::weak_ptr::lock returns empty std::shared_ptr.
                //   - The MqttConnection object destructor called.
                // The first scenario extends the MqttConnection object lifetime for the next user callback execution.
                // With the second scenario, the user callback for which we try to obtain the MqttConnection instance,
                // won't be called.
                return m_connection.lock();
            }

            void MqttConnectionCore::s_onWebsocketHandshake(
                struct aws_http_message *rawRequest,
                void *userData,
                aws_mqtt_transform_websocket_handshake_complete_fn *completeFn,
                void *completeCtx)
            {
                auto *connectionCore = reinterpret_cast<MqttConnectionCore *>(userData);
                auto connection = connectionCore->obtainConnectionInstance();
                if (!connection)
                {
                    return;
                }

                // At this point we ensured that the MqttConnection object will be alive for the duration of the
                // callback execution, so no critical section is needed.

                Allocator *allocator = connectionCore->m_allocator;
                // we have to do this because of private constructors.
                auto *toSeat =
                    reinterpret_cast<Http::HttpRequest *>(aws_mem_acquire(allocator, sizeof(Http::HttpRequest)));
                toSeat = new (toSeat) Http::HttpRequest(allocator, rawRequest);

                auto request = std::shared_ptr<Http::HttpRequest>(
                    toSeat, [allocator](Http::HttpRequest *ptr) { Crt::Delete(ptr, allocator); });

                auto onInterceptComplete =
                    [completeFn,
                     completeCtx](const std::shared_ptr<Http::HttpRequest> &transformedRequest, int errorCode)
                { completeFn(transformedRequest->GetUnderlyingMessage(), errorCode, completeCtx); };

                if (connection->WebsocketInterceptor)
                {
                    connection->WebsocketInterceptor(request, onInterceptComplete);
                }
            }

            MqttConnectionCore::operator bool() const noexcept
            {
                return m_underlyingConnection != nullptr;
            }

            void MqttConnectionCore::Destroy()
            {
                if (*this)
                {
                    // Initiate disconnect in case we currently connected.
                    Disconnect();

                    // Initiate the destruction process of the underlying connection.
                    // MqttConnectionCore::s_onConnectionTermination will be called asynchronously (through
                    // on_connection_termination handler of the underlying connection) when the destruction process is
                    // completed.
                    aws_mqtt_client_connection_release(m_underlyingConnection);
                }
            }

            int MqttConnectionCore::LastError() const noexcept
            {
                return aws_last_error();
            }

            bool MqttConnectionCore::SetWill(const char *topic, QOS qos, bool retain, const ByteBuf &payload) noexcept
            {
                ByteBuf topicBuf = aws_byte_buf_from_c_str(topic);
                ByteCursor topicCur = aws_byte_cursor_from_buf(&topicBuf);
                ByteCursor payloadCur = aws_byte_cursor_from_buf(&payload);

                return aws_mqtt_client_connection_set_will(
                           m_underlyingConnection, &topicCur, qos, retain, &payloadCur) == 0;
            }

            bool MqttConnectionCore::SetLogin(const char *username, const char *password) noexcept
            {
                ByteBuf usernameBuf = aws_byte_buf_from_c_str(username);
                ByteCursor usernameCur = aws_byte_cursor_from_buf(&usernameBuf);

                ByteCursor *pwdCurPtr = nullptr;
                ByteCursor pwdCur;

                if (password != nullptr)
                {
                    pwdCur = ByteCursorFromCString(password);
                    pwdCurPtr = &pwdCur;
                }
                return aws_mqtt_client_connection_set_login(m_underlyingConnection, &usernameCur, pwdCurPtr) == 0;
            }

            bool MqttConnectionCore::SetHttpProxyOptions(
                const Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept
            {
                m_proxyOptions = proxyOptions;
                return true;
            }

            bool MqttConnectionCore::SetReconnectTimeout(uint64_t min_seconds, uint64_t max_seconds) noexcept
            {
                return aws_mqtt_client_connection_set_reconnect_timeout(
                           m_underlyingConnection, min_seconds, max_seconds) == 0;
            }

            bool MqttConnectionCore::Connect(
                const char *clientId,
                bool cleanSession,
                uint16_t keepAliveTime,
                uint32_t pingTimeoutMs,
                uint32_t protocolOperationTimeoutMs,
                bool setWebSocketInterceptor) noexcept
            {
                aws_mqtt_connection_options options;
                AWS_ZERO_STRUCT(options);
                options.client_id = aws_byte_cursor_from_c_str(clientId);
                options.host_name = aws_byte_cursor_from_array(
                    reinterpret_cast<const uint8_t *>(m_hostName.data()), m_hostName.length());
                options.tls_options =
                    m_useTls ? const_cast<aws_tls_connection_options *>(m_tlsOptions.GetUnderlyingHandle()) : nullptr;
                options.port = m_port;
                options.socket_options = &m_socketOptions.GetImpl();
                options.clean_session = cleanSession;
                options.keep_alive_time_secs = keepAliveTime;
                options.ping_timeout_ms = pingTimeoutMs;
                options.protocol_operation_timeout_ms = protocolOperationTimeoutMs;
                options.on_connection_complete = MqttConnectionCore::s_onConnectionCompleted;
                options.user_data = this;

                if (m_useWebsocket)
                {
                    if (setWebSocketInterceptor)
                    {
                        if (aws_mqtt_client_connection_use_websockets(
                                m_underlyingConnection,
                                MqttConnectionCore::s_onWebsocketHandshake,
                                this,
                                nullptr,
                                nullptr) != 0)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        if (aws_mqtt_client_connection_use_websockets(
                                m_underlyingConnection, nullptr, nullptr, nullptr, nullptr) != 0)
                        {
                            return false;
                        }
                    }
                }

                if (m_proxyOptions)
                {
                    struct aws_http_proxy_options proxyOptions;
                    m_proxyOptions->InitializeRawProxyOptions(proxyOptions);

                    if (aws_mqtt_client_connection_set_http_proxy_options(m_underlyingConnection, &proxyOptions) != 0)
                    {
                        return false;
                    }
                }

                return aws_mqtt_client_connection_connect(m_underlyingConnection, &options) == AWS_OP_SUCCESS;
            }

            bool MqttConnectionCore::Disconnect() noexcept
            {
                return aws_mqtt_client_connection_disconnect(
                           m_underlyingConnection, MqttConnectionCore::s_onDisconnect, this) == AWS_OP_SUCCESS;
            }

            aws_mqtt_client_connection *MqttConnectionCore::GetUnderlyingConnection() const noexcept
            {
                return m_underlyingConnection;
            }

            bool MqttConnectionCore::SetOnMessageHandler(OnMessageReceivedHandler &&onMessage) noexcept
            {
                auto *pubCallbackData = Aws::Crt::New<PubCallbackData>(m_allocator);
                if (pubCallbackData == nullptr)
                {
                    return false;
                }

                pubCallbackData->connectionCore = this;
                pubCallbackData->onMessageReceived = std::move(onMessage);
                pubCallbackData->allocator = m_allocator;

                if (aws_mqtt_client_connection_set_on_any_publish_handler(
                        m_underlyingConnection, s_onPublish, pubCallbackData) == 0)
                {
                    // There is a previously set message handler. We can delete it safely only after setting a new
                    // handler successfully.
                    if (m_onAnyCbData != nullptr)
                    {
                        auto *previousData = reinterpret_cast<PubCallbackData *>(m_onAnyCbData);
                        Crt::Delete(previousData, previousData->allocator);
                    }
                    m_onAnyCbData = reinterpret_cast<void *>(pubCallbackData);
                    return true;
                }

                Aws::Crt::Delete(pubCallbackData, pubCallbackData->allocator);
                return false;
            }

            uint16_t MqttConnectionCore::Subscribe(
                const char *topicFilter,
                QOS qos,
                OnMessageReceivedHandler &&onMessage,
                OnSubAckHandler &&onSubAck) noexcept
            {
                auto *pubCallbackData = Crt::New<PubCallbackData>(m_allocator);

                if (pubCallbackData == nullptr)
                {
                    return 0;
                }

                pubCallbackData->connectionCore = this;
                pubCallbackData->onMessageReceived = std::move(onMessage);
                pubCallbackData->allocator = m_allocator;

                auto *subAckCallbackData = Crt::New<SubAckCallbackData>(m_allocator);

                if (subAckCallbackData == nullptr)
                {
                    Crt::Delete(pubCallbackData, m_allocator);
                    return 0;
                }

                subAckCallbackData->connectionCore = this;
                subAckCallbackData->allocator = m_allocator;
                subAckCallbackData->onSubAck = std::move(onSubAck);
                subAckCallbackData->topic = nullptr;
                subAckCallbackData->allocator = m_allocator;

                ByteBuf topicFilterBuf = aws_byte_buf_from_c_str(topicFilter);
                ByteCursor topicFilterCur = aws_byte_cursor_from_buf(&topicFilterBuf);

                uint16_t packetId = aws_mqtt_client_connection_subscribe(
                    m_underlyingConnection,
                    &topicFilterCur,
                    qos,
                    s_onPublish,
                    pubCallbackData,
                    s_cleanUpOnPublishData,
                    s_onSubAck,
                    subAckCallbackData);

                if (packetId == 0U)
                {
                    Crt::Delete(pubCallbackData, pubCallbackData->allocator);
                    Crt::Delete(subAckCallbackData, subAckCallbackData->allocator);
                }

                return packetId;
            }

            uint16_t MqttConnectionCore::Subscribe(
                const Vector<std::pair<const char *, OnMessageReceivedHandler>> &topicFilters,
                QOS qos,
                OnMultiSubAckHandler &&onOpComplete) noexcept
            {
                uint16_t packetId = 0;
                auto *subAckCallbackData = Crt::New<MultiSubAckCallbackData>(m_allocator);

                if (subAckCallbackData == nullptr)
                {
                    return 0;
                }

                aws_array_list multiPub;
                AWS_ZERO_STRUCT(multiPub);

                if (aws_array_list_init_dynamic(
                        &multiPub, m_allocator, topicFilters.size(), sizeof(aws_mqtt_topic_subscription)) != 0)
                {
                    Crt::Delete(subAckCallbackData, m_allocator);
                    return 0;
                }

                bool errorOccurred = false;
                for (const auto &topicFilter : topicFilters)
                {
                    auto *pubCallbackData = Crt::New<PubCallbackData>(m_allocator);

                    if (pubCallbackData == nullptr)
                    {
                        errorOccurred = true;
                        break;
                    }

                    pubCallbackData->connectionCore = this;
                    pubCallbackData->onMessageReceived = topicFilter.second;
                    pubCallbackData->allocator = m_allocator;

                    ByteBuf topicFilterBuf = aws_byte_buf_from_c_str(topicFilter.first);
                    ByteCursor topicFilterCur = aws_byte_cursor_from_buf(&topicFilterBuf);

                    aws_mqtt_topic_subscription subscription;
                    subscription.on_cleanup = s_cleanUpOnPublishData;
                    subscription.on_publish = s_onPublish;
                    subscription.on_publish_ud = pubCallbackData;
                    subscription.qos = qos;
                    subscription.topic = topicFilterCur;

                    if (aws_array_list_push_back(&multiPub, reinterpret_cast<const void *>(&subscription)) != 0)
                    {
                        Crt::Delete(pubCallbackData, m_allocator);
                        errorOccurred = true;
                        break;
                    }
                }

                if (!errorOccurred)
                {
                    subAckCallbackData->connectionCore = this;
                    subAckCallbackData->allocator = m_allocator;
                    subAckCallbackData->onSubAck = std::move(onOpComplete);
                    subAckCallbackData->topic = nullptr;
                    subAckCallbackData->allocator = m_allocator;

                    packetId = aws_mqtt_client_connection_subscribe_multiple(
                        m_underlyingConnection, &multiPub, s_onMultiSubAck, subAckCallbackData);
                }

                if (packetId == 0U)
                {
                    size_t length = aws_array_list_length(&multiPub);
                    for (size_t i = 0; i < length; ++i)
                    {
                        aws_mqtt_topic_subscription *subscription = nullptr;
                        aws_array_list_get_at_ptr(&multiPub, reinterpret_cast<void **>(&subscription), i);
                        auto *pubCallbackData = reinterpret_cast<PubCallbackData *>(subscription->on_publish_ud);
                        Crt::Delete(pubCallbackData, m_allocator);
                    }

                    Crt::Delete(subAckCallbackData, m_allocator);
                }

                aws_array_list_clean_up(&multiPub);

                return packetId;
            }

            uint16_t MqttConnectionCore::Unsubscribe(
                const char *topicFilter,
                OnOperationCompleteHandler &&onOpComplete) noexcept
            {
                auto *opCompleteCallbackData = Crt::New<OpCompleteCallbackData>(m_allocator);

                if (opCompleteCallbackData == nullptr)
                {
                    return 0;
                }

                opCompleteCallbackData->connectionCore = this;
                opCompleteCallbackData->allocator = m_allocator;
                opCompleteCallbackData->onOperationComplete = std::move(onOpComplete);
                ByteBuf topicFilterBuf = aws_byte_buf_from_c_str(topicFilter);
                ByteCursor topicFilterCur = aws_byte_cursor_from_buf(&topicFilterBuf);

                uint16_t packetId = aws_mqtt_client_connection_unsubscribe(
                    m_underlyingConnection, &topicFilterCur, s_onOpComplete, opCompleteCallbackData);

                if (packetId == 0U)
                {
                    Crt::Delete(opCompleteCallbackData, m_allocator);
                }

                return packetId;
            }

            uint16_t MqttConnectionCore::Publish(
                const char *topic,
                QOS qos,
                bool retain,
                const ByteBuf &payload,
                OnOperationCompleteHandler &&onOpComplete) noexcept
            {

                auto *opCompleteCallbackData = Crt::New<OpCompleteCallbackData>(m_allocator);
                if (opCompleteCallbackData == nullptr)
                {
                    return 0;
                }

                opCompleteCallbackData->connectionCore = this;
                opCompleteCallbackData->allocator = m_allocator;
                opCompleteCallbackData->onOperationComplete = std::move(onOpComplete);

                ByteCursor topicCur = aws_byte_cursor_from_array(topic, strnlen(topic, AWS_MQTT_MAX_TOPIC_LENGTH));
                ByteCursor payloadCur = aws_byte_cursor_from_buf(&payload);
                uint16_t packetId = aws_mqtt_client_connection_publish(
                    m_underlyingConnection,
                    &topicCur,
                    qos,
                    retain,
                    &payloadCur,
                    s_onOpComplete,
                    opCompleteCallbackData);

                if (packetId == 0U)
                {
                    Crt::Delete(opCompleteCallbackData, m_allocator);
                }

                return packetId;
            }

            const MqttConnectionOperationStatistics &MqttConnectionCore::GetOperationStatistics() noexcept
            {
                aws_mqtt_connection_operation_statistics operationStatisticsNative = {0, 0, 0, 0};
                if (m_underlyingConnection != nullptr)
                {
                    aws_mqtt_client_connection_get_stats(m_underlyingConnection, &operationStatisticsNative);
                    m_operationStatistics.incompleteOperationCount =
                        operationStatisticsNative.incomplete_operation_count;
                    m_operationStatistics.incompleteOperationSize = operationStatisticsNative.incomplete_operation_size;
                    m_operationStatistics.unackedOperationCount = operationStatisticsNative.unacked_operation_count;
                    m_operationStatistics.unackedOperationSize = operationStatisticsNative.unacked_operation_size;
                }
                return m_operationStatistics;
            }
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
/*! \endcond */
