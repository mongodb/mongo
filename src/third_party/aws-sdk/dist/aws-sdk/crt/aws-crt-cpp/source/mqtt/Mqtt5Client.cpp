/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>
#include <aws/crt/mqtt/private/Mqtt5ClientCore.h>

#include <aws/crt/Api.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/http/HttpProxyStrategy.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/iot/MqttClient.h>

#include <utility>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            Mqtt5Client::Mqtt5Client(const Mqtt5ClientOptions &options, Allocator *allocator) noexcept
                : m_client_core(nullptr)
            {
                m_client_core = Mqtt5ClientCore::NewMqtt5ClientCore(options, allocator);
            }

            Mqtt5Client::~Mqtt5Client()
            {
                if (m_client_core != nullptr)
                {
                    m_client_core->Close();
                    m_client_core.reset();
                }
            }

            std::shared_ptr<Mqtt5Client> Mqtt5Client::NewMqtt5Client(
                const Mqtt5ClientOptions &options,
                Allocator *allocator) noexcept
            {
                /* Copied from MqttClient.cpp:ln754 */
                // As the constructor is private, make share would not work here. We do make_share manually.
                Mqtt5Client *toSeat = reinterpret_cast<Mqtt5Client *>(aws_mem_acquire(allocator, sizeof(Mqtt5Client)));
                if (!toSeat)
                {
                    return nullptr;
                }

                toSeat = new (toSeat) Mqtt5Client(options, allocator);

                // Creation failed, make sure we release the allocated memory
                if (!*toSeat)
                {
                    Crt::Delete(toSeat, allocator);
                    return nullptr;
                }

                return std::shared_ptr<Mqtt5Client>(
                    toSeat, [allocator](Mqtt5Client *client) { Crt::Delete(client, allocator); });
            }

            Mqtt5Client::operator bool() const noexcept
            {
                return m_client_core != nullptr;
            }

            int Mqtt5Client::LastError() const noexcept
            {
                return aws_last_error();
            }

            bool Mqtt5Client::Start() const noexcept
            {
                if (m_client_core == nullptr)
                {
                    AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "Failed to start the client: Mqtt5 Client is invalid.");
                    return false;
                }
                return aws_mqtt5_client_start(m_client_core->m_client) == AWS_OP_SUCCESS;
            }

            bool Mqtt5Client::Stop() noexcept
            {
                if (m_client_core == nullptr)
                {
                    AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "Failed to stop the client: Mqtt5 Client is invalid.");
                    return false;
                }
                return aws_mqtt5_client_stop(m_client_core->m_client, NULL, NULL) == AWS_OP_SUCCESS;
            }

            bool Mqtt5Client::Stop(std::shared_ptr<DisconnectPacket> disconnectOptions) noexcept
            {
                if (m_client_core == nullptr)
                {
                    AWS_LOGF_DEBUG(AWS_LS_MQTT5_CLIENT, "Failed to stop the client: Mqtt5 Client is invalid.");
                    return false;
                }
                if (disconnectOptions == nullptr)
                {
                    return Stop();
                }

                aws_mqtt5_packet_disconnect_view disconnect_packet;
                AWS_ZERO_STRUCT(disconnect_packet);
                if (disconnectOptions->initializeRawOptions(disconnect_packet) == false)
                {
                    return false;
                }
                return aws_mqtt5_client_stop(m_client_core->m_client, &disconnect_packet, NULL) == AWS_OP_SUCCESS;
            }

            bool Mqtt5Client::Publish(
                std::shared_ptr<PublishPacket> publishOptions,
                OnPublishCompletionHandler onPublishCompletionCallback) noexcept
            {
                if (m_client_core == nullptr || publishOptions == nullptr)
                {
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT5_CLIENT, "Failed to publish: the Mqtt5 client or the publish option is invalid.");
                    return false;
                }

                /* The callbacks should be handled by the client core*/
                return m_client_core->Publish(publishOptions, onPublishCompletionCallback);
            }

            bool Mqtt5Client::Subscribe(
                std::shared_ptr<SubscribePacket> subscribeOptions,
                OnSubscribeCompletionHandler onSubscribeCompletionCallback) noexcept
            {
                if (m_client_core == nullptr || subscribeOptions == nullptr)
                {
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT5_CLIENT,
                        "Failed to subscribe: the Mqtt5 client or the subscribe option is invalid.");
                    return false;
                }
                /* The callbacks should be handled by the client core*/
                return m_client_core->Subscribe(subscribeOptions, onSubscribeCompletionCallback);
            }

            bool Mqtt5Client::Unsubscribe(
                std::shared_ptr<UnsubscribePacket> unsubscribeOptions,
                OnUnsubscribeCompletionHandler onUnsubscribeCompletionCallback) noexcept
            {
                if (m_client_core == nullptr || unsubscribeOptions == nullptr)
                {
                    AWS_LOGF_DEBUG(
                        AWS_LS_MQTT5_CLIENT,
                        "Failed to unsubscribe: the Mqtt5 client or the unsubscribe option is invalid.");
                    return false;
                }

                /* The callbacks should be handled by the client core*/
                return m_client_core->Unsubscribe(unsubscribeOptions, onUnsubscribeCompletionCallback);
            }

            const Mqtt5ClientOperationStatistics &Mqtt5Client::GetOperationStatistics() noexcept
            {
                aws_mqtt5_client_operation_statistics m_operationStatisticsNative = {0, 0, 0, 0};
                if (m_client_core != nullptr)
                {
                    aws_mqtt5_client_get_stats(m_client_core->m_client, &m_operationStatisticsNative);
                    m_operationStatistics.incompleteOperationCount =
                        m_operationStatisticsNative.incomplete_operation_count;
                    m_operationStatistics.incompleteOperationSize =
                        m_operationStatisticsNative.incomplete_operation_size;
                    m_operationStatistics.unackedOperationCount = m_operationStatisticsNative.unacked_operation_count;
                    m_operationStatistics.unackedOperationSize = m_operationStatisticsNative.unacked_operation_size;
                }
                return m_operationStatistics;
            }

            struct aws_mqtt5_client *Mqtt5Client::GetUnderlyingHandle() const noexcept
            {
                return m_client_core->GetUnderlyingHandle();
            }

            /*****************************************************
             *
             * Mqtt5ClientOptions
             *
             *****************************************************/

            /**
             * Mqtt5ClientOptions
             */
            Mqtt5ClientOptions::Mqtt5ClientOptions(Crt::Allocator *allocator) noexcept
                : m_bootstrap(nullptr), m_sessionBehavior(ClientSessionBehaviorType::AWS_MQTT5_CSBT_DEFAULT),
                  m_extendedValidationAndFlowControlOptions(AWS_MQTT5_EVAFCO_AWS_IOT_CORE_DEFAULTS),
                  m_offlineQueueBehavior(AWS_MQTT5_COQBT_DEFAULT),
                  m_reconnectionOptions({AWS_EXPONENTIAL_BACKOFF_JITTER_DEFAULT, 0, 0, 0}), m_pingTimeoutMs(0),
                  m_connackTimeoutMs(0), m_ackTimeoutSec(0), m_allocator(allocator)
            {
                m_socketOptions.SetSocketType(Io::SocketType::Stream);
                AWS_ZERO_STRUCT(m_packetConnectViewStorage);
                AWS_ZERO_STRUCT(m_httpProxyOptionsStorage);

                AWS_ZERO_STRUCT(m_topicAliasingOptions);
            }

            bool Mqtt5ClientOptions::initializeRawOptions(aws_mqtt5_client_options &raw_options) const noexcept
            {
                AWS_ZERO_STRUCT(raw_options);

                raw_options.host_name = ByteCursorFromString(m_hostName);
                raw_options.port = m_port;

                if (m_bootstrap == nullptr)
                {
                    raw_options.bootstrap = ApiHandle::GetOrCreateStaticDefaultClientBootstrap()->GetUnderlyingHandle();
                }
                else
                {
                    raw_options.bootstrap = m_bootstrap->GetUnderlyingHandle();
                }
                raw_options.socket_options = &m_socketOptions.GetImpl();
                if (m_tlsConnectionOptions.has_value())
                {
                    raw_options.tls_options = m_tlsConnectionOptions.value().GetUnderlyingHandle();
                }

                if (m_proxyOptions.has_value())
                {
                    raw_options.http_proxy_options = &m_httpProxyOptionsStorage;
                }

                raw_options.connect_options = &m_packetConnectViewStorage;
                raw_options.session_behavior = m_sessionBehavior;
                raw_options.extended_validation_and_flow_control_options = m_extendedValidationAndFlowControlOptions;
                raw_options.offline_queue_behavior = m_offlineQueueBehavior;
                raw_options.retry_jitter_mode = m_reconnectionOptions.m_reconnectMode;
                raw_options.max_reconnect_delay_ms = m_reconnectionOptions.m_maxReconnectDelayMs;
                raw_options.min_reconnect_delay_ms = m_reconnectionOptions.m_minReconnectDelayMs;
                raw_options.min_connected_time_to_reset_reconnect_delay_ms =
                    m_reconnectionOptions.m_minConnectedTimeToResetReconnectDelayMs;
                raw_options.ping_timeout_ms = m_pingTimeoutMs;
                raw_options.connack_timeout_ms = m_connackTimeoutMs;
                raw_options.ack_timeout_seconds = m_ackTimeoutSec;
                raw_options.topic_aliasing_options = &m_topicAliasingOptions;

                return true;
            }

            Mqtt5ClientOptions::~Mqtt5ClientOptions() {}

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithHostName(Crt::String hostname)
            {
                m_hostName = std::move(hostname);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithPort(uint32_t port) noexcept
            {
                m_port = port;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithBootstrap(Io::ClientBootstrap *bootStrap) noexcept
            {
                m_bootstrap = bootStrap;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithSocketOptions(Io::SocketOptions socketOptions) noexcept
            {
                m_socketOptions = std::move(socketOptions);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithTlsConnectionOptions(
                const Io::TlsConnectionOptions &tslOptions) noexcept
            {
                m_tlsConnectionOptions = tslOptions;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithHttpProxyOptions(
                const Crt::Http::HttpClientConnectionProxyOptions &proxyOptions) noexcept
            {
                m_proxyOptions = proxyOptions;
                m_proxyOptions->InitializeRawProxyOptions(m_httpProxyOptionsStorage);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithConnectOptions(
                std::shared_ptr<ConnectPacket> packetConnect) noexcept
            {
                m_connectOptions = packetConnect;
                m_connectOptions->initializeRawOptions(m_packetConnectViewStorage, m_allocator);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithSessionBehavior(
                ClientSessionBehaviorType sessionBehavior) noexcept
            {
                m_sessionBehavior = sessionBehavior;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientExtendedValidationAndFlowControl(
                ClientExtendedValidationAndFlowControl clientExtendedValidationAndFlowControl) noexcept
            {
                m_extendedValidationAndFlowControlOptions = clientExtendedValidationAndFlowControl;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithOfflineQueueBehavior(
                ClientOperationQueueBehaviorType offlineQueueBehavior) noexcept
            {
                m_offlineQueueBehavior = offlineQueueBehavior;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithReconnectOptions(ReconnectOptions reconnectOptions) noexcept
            {
                m_reconnectionOptions = reconnectOptions;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithTopicAliasingOptions(
                TopicAliasingOptions topicAliasingOptions) noexcept
            {
                m_topicAliasingOptions.outbound_topic_alias_behavior =
                    topicAliasingOptions.m_outboundBehavior.has_value()
                        ? (enum aws_mqtt5_client_outbound_topic_alias_behavior_type)
                              topicAliasingOptions.m_outboundBehavior.value()
                        : AWS_MQTT5_COTABT_DEFAULT;
                m_topicAliasingOptions.outbound_alias_cache_max_size =
                    topicAliasingOptions.m_outboundCacheMaxSize.has_value()
                        ? topicAliasingOptions.m_outboundCacheMaxSize.value()
                        : (uint16_t)0;
                m_topicAliasingOptions.inbound_topic_alias_behavior =
                    topicAliasingOptions.m_inboundBehavior.has_value()
                        ? (enum aws_mqtt5_client_inbound_topic_alias_behavior_type)
                              topicAliasingOptions.m_inboundBehavior.value()
                        : AWS_MQTT5_CITABT_DEFAULT;
                m_topicAliasingOptions.inbound_alias_cache_size =
                    topicAliasingOptions.m_inboundCacheMaxSize.has_value()
                        ? topicAliasingOptions.m_inboundCacheMaxSize.value()
                        : (uint16_t)0;

                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithPingTimeoutMs(uint32_t pingTimeoutMs) noexcept
            {
                m_pingTimeoutMs = pingTimeoutMs;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithConnackTimeoutMs(uint32_t connackTimeoutMs) noexcept
            {
                m_connackTimeoutMs = connackTimeoutMs;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithAckTimeoutSeconds(uint32_t ackTimeoutSec) noexcept
            {
                return WithAckTimeoutSec(ackTimeoutSec);
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithAckTimeoutSec(uint32_t ackTimeoutSec) noexcept
            {
                m_ackTimeoutSec = ackTimeoutSec;
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithWebsocketHandshakeTransformCallback(
                OnWebSocketHandshakeIntercept callback) noexcept
            {
                websocketHandshakeTransform = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientConnectionSuccessCallback(
                OnConnectionSuccessHandler callback) noexcept
            {
                onConnectionSuccess = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientConnectionFailureCallback(
                OnConnectionFailureHandler callback) noexcept
            {
                onConnectionFailure = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientDisconnectionCallback(
                OnDisconnectionHandler callback) noexcept
            {
                onDisconnection = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientStoppedCallback(OnStoppedHandler callback) noexcept
            {
                onStopped = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithClientAttemptingConnectCallback(
                OnAttemptingConnectHandler callback) noexcept
            {
                onAttemptingConnect = std::move(callback);
                return *this;
            }

            Mqtt5ClientOptions &Mqtt5ClientOptions::WithPublishReceivedCallback(
                OnPublishReceivedHandler callback) noexcept
            {
                onPublishReceived = std::move(callback);
                return *this;
            }

        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
