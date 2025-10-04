/*! \cond DOXYGEN_PRIVATE
** Hide API from this file in doxygen. Set DOXYGEN_PRIVATE in doxygen
** config to enable this file for doxygen.
*/
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>
#include <aws/crt/mqtt/private/Mqtt5ClientCore.h>

#include <aws/crt/Api.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/http/HttpRequestResponse.h>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            struct PubAckCallbackData : public std::enable_shared_from_this<PubAckCallbackData>
            {
                PubAckCallbackData(Allocator *alloc = ApiAllocator()) : clientCore(nullptr), allocator(alloc) {}

                Mqtt5ClientCore *clientCore;
                OnPublishCompletionHandler onPublishCompletion;
                Allocator *allocator;
            };

            struct SubAckCallbackData
            {
                SubAckCallbackData(Allocator *alloc = ApiAllocator()) : clientCore(nullptr), allocator(alloc) {}

                Mqtt5ClientCore *clientCore;
                OnSubscribeCompletionHandler onSubscribeCompletion;
                Allocator *allocator;
            };

            struct UnSubAckCallbackData
            {
                UnSubAckCallbackData(Allocator *alloc = ApiAllocator()) : clientCore(nullptr), allocator(alloc) {}
                Mqtt5ClientCore *clientCore;
                OnUnsubscribeCompletionHandler onUnsubscribeCompletion;
                Allocator *allocator;
            };

            void Mqtt5ClientCore::s_lifeCycleEventCallback(const struct aws_mqtt5_client_lifecycle_event *event)
            {
                Mqtt5ClientCore *client_core = reinterpret_cast<Mqtt5ClientCore *>(event->user_data);
                if (client_core == nullptr)
                {
                    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Lifecycle event: error retrieving callback userdata. ");
                    return;
                }

                std::lock_guard<std::recursive_mutex> lock(client_core->m_callback_lock);
                if (client_core->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                {
                    AWS_LOGF_INFO(
                        AWS_LS_MQTT5_CLIENT, "Lifecycle event: mqtt5 client is not valid, revoke the callbacks.");
                    return;
                }

                switch (event->event_type)
                {
                    case AWS_MQTT5_CLET_STOPPED:
                        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Lifecycle event: Client Stopped!");
                        if (client_core->onStopped != nullptr)
                        {
                            OnStoppedEventData eventData;
                            client_core->onStopped(eventData);
                        }
                        break;

                    case AWS_MQTT5_CLET_ATTEMPTING_CONNECT:
                        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Lifecycle event: Attempting Connect!");
                        if (client_core->onAttemptingConnect != nullptr)
                        {
                            OnAttemptingConnectEventData eventData;
                            client_core->onAttemptingConnect(eventData);
                        }
                        break;

                    case AWS_MQTT5_CLET_CONNECTION_FAILURE:
                        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Lifecycle event: Connection Failure!");
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "  Error Code: %d(%s)",
                            event->error_code,
                            aws_error_debug_str(event->error_code));
                        if (client_core->onConnectionFailure != nullptr)
                        {
                            OnConnectionFailureEventData eventData;
                            eventData.errorCode = event->error_code;
                            std::shared_ptr<ConnAckPacket> packet = nullptr;
                            if (event->connack_data != nullptr)
                            {
                                packet = Aws::Crt::MakeShared<ConnAckPacket>(
                                    client_core->m_allocator, *event->connack_data, client_core->m_allocator);
                                eventData.connAckPacket = packet;
                            }
                            client_core->onConnectionFailure(eventData);
                        }
                        break;

                    case AWS_MQTT5_CLET_CONNECTION_SUCCESS:
                        AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Lifecycle event: Connection Success!");
                        if (client_core->onConnectionSuccess != nullptr)
                        {
                            OnConnectionSuccessEventData eventData;

                            std::shared_ptr<ConnAckPacket> packet = nullptr;
                            if (event->connack_data != nullptr)
                            {
                                packet = Aws::Crt::MakeShared<ConnAckPacket>(ApiAllocator(), *event->connack_data);
                            }

                            std::shared_ptr<NegotiatedSettings> neg_settings = nullptr;
                            if (event->settings != nullptr)
                            {
                                neg_settings =
                                    Aws::Crt::MakeShared<NegotiatedSettings>(ApiAllocator(), *event->settings);
                            }

                            eventData.connAckPacket = packet;
                            eventData.negotiatedSettings = neg_settings;
                            client_core->onConnectionSuccess(eventData);
                        }
                        break;

                    case AWS_MQTT5_CLET_DISCONNECTION:
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "  Error Code: %d(%s)",
                            event->error_code,
                            aws_error_debug_str(event->error_code));
                        if (client_core->onDisconnection != nullptr)
                        {
                            OnDisconnectionEventData eventData;
                            std::shared_ptr<DisconnectPacket> disconnection = nullptr;
                            if (event->disconnect_data != nullptr)
                            {
                                disconnection = Aws::Crt::MakeShared<DisconnectPacket>(
                                    client_core->m_allocator, *event->disconnect_data, client_core->m_allocator);
                            }
                            eventData.errorCode = event->error_code;
                            eventData.disconnectPacket = disconnection;
                            client_core->onDisconnection(eventData);
                        }
                        break;
                }
            }

            void Mqtt5ClientCore::s_publishReceivedCallback(
                const struct aws_mqtt5_packet_publish_view *publish,
                void *user_data)
            {
                AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Publish Received Event: on publish received callback");
                Mqtt5ClientCore *client_core = reinterpret_cast<Mqtt5ClientCore *>(user_data);

                if (client_core == nullptr)
                {
                    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Publish Received Event: error retrieving callback userdata. ");
                    return;
                }

                /* Callback not set */
                if (client_core->onPublishReceived == nullptr)
                {
                    return;
                }

                std::lock_guard<std::recursive_mutex> lock(client_core->m_callback_lock);
                if (client_core->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                {
                    AWS_LOGF_INFO(
                        AWS_LS_MQTT5_CLIENT,
                        "Publish Received Event: mqtt5 client is not valid, revoke the callbacks.");
                    return;
                }

                if (client_core->onPublishReceived != nullptr)
                {
                    if (publish != nullptr)
                    {
                        std::shared_ptr<PublishPacket> packet =
                            std::make_shared<PublishPacket>(*publish, client_core->m_allocator);
                        PublishReceivedEventData eventData;
                        eventData.publishPacket = packet;
                        client_core->onPublishReceived(eventData);
                    }
                    else
                    {
                        AWS_LOGF_ERROR(
                            AWS_LS_MQTT5_CLIENT, "Publish Received Event: Failed to access Publish packet view.");
                    }
                }
            }

            void Mqtt5ClientCore::s_publishCompletionCallback(
                enum aws_mqtt5_packet_type packet_type,
                const void *publishCompletionPacket,
                int error_code,
                void *complete_ctx)
            {
                AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Publish completion callback triggered.");
                auto callbackData = reinterpret_cast<PubAckCallbackData *>(complete_ctx);
                AWS_ASSERT(callbackData != nullptr);
                AWS_ASSERT(callbackData->clientCore != nullptr);

                /* callback not set */
                if (callbackData->onPublishCompletion == nullptr)
                {
                    goto on_publishCompletionCleanup;
                }

                {
                    std::lock_guard<std::recursive_mutex> lock(callbackData->clientCore->m_callback_lock);
                    if (callbackData->clientCore->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                    {
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "Publish Completion Callback: mqtt5 client is not valid, revoke the callbacks.");
                        goto on_publishCompletionCleanup;
                    }
                }

                {
                    std::shared_ptr<PublishResult> publish = nullptr;
                    switch (packet_type)
                    {
                        case aws_mqtt5_packet_type::AWS_MQTT5_PT_PUBACK:
                        {
                            if (publishCompletionPacket != nullptr)
                            {
                                std::shared_ptr<PubAckPacket> packet = std::make_shared<PubAckPacket>(
                                    *(aws_mqtt5_packet_puback_view *)publishCompletionPacket, callbackData->allocator);
                                publish = std::make_shared<PublishResult>(std::move(packet));
                            }
                            else /* This should never happened. */
                            {
                                AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "The PubAck Packet is null.");
                                AWS_FATAL_ASSERT(!"The PubAck Packet is invalid.");
                            }
                            break;
                        }
                        case aws_mqtt5_packet_type::AWS_MQTT5_PT_NONE:
                        {
                            publish = std::make_shared<PublishResult>(error_code);
                            break;
                        }
                        default: /* Invalid packet type */
                        {
                            AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Invalid Packet Type.");
                            publish = std::make_shared<PublishResult>(AWS_ERROR_UNKNOWN);
                            break;
                        }
                    }
                    callbackData->onPublishCompletion(error_code, publish);
                }
            on_publishCompletionCleanup:
                Crt::Delete(callbackData, callbackData->allocator);
            }

            void Mqtt5ClientCore::s_onWebsocketHandshake(
                struct aws_http_message *rawRequest,
                void *user_data,
                aws_mqtt5_transform_websocket_handshake_complete_fn *complete_fn,
                void *complete_ctx)
            {
                auto client_core = reinterpret_cast<Mqtt5ClientCore *>(user_data);
                if (client_core == nullptr)
                {
                    AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Websocket Handshake: error retrieving callback userdata. ");
                    return;
                }

                /* The websocketInterceptor must be set */
                AWS_FATAL_ASSERT(client_core->websocketInterceptor);

                std::lock_guard<std::recursive_mutex> lock(client_core->m_callback_lock);
                if (client_core->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                {
                    AWS_LOGF_INFO(
                        AWS_LS_MQTT5_CLIENT, "Websocket Handshake: mqtt5 client is not valid, revoke the callbacks.");
                    return;
                }

                Allocator *allocator = client_core->m_allocator;
                // we have to do this because of private constructors.
                auto toSeat =
                    reinterpret_cast<Http::HttpRequest *>(aws_mem_acquire(allocator, sizeof(Http::HttpRequest)));
                toSeat = new (toSeat) Http::HttpRequest(allocator, rawRequest);

                std::shared_ptr<Http::HttpRequest> request = std::shared_ptr<Http::HttpRequest>(
                    toSeat, [allocator](Http::HttpRequest *ptr) { Crt::Delete(ptr, allocator); });

                auto onInterceptComplete =
                    [complete_fn,
                     complete_ctx](const std::shared_ptr<Http::HttpRequest> &transformedRequest, int errorCode)
                { complete_fn(transformedRequest->GetUnderlyingMessage(), errorCode, complete_ctx); };

                client_core->websocketInterceptor(request, onInterceptComplete);
            }

            void Mqtt5ClientCore::s_clientTerminationCompletion(void *complete_ctx)
            {
                Mqtt5ClientCore *client_core = reinterpret_cast<Mqtt5ClientCore *>(complete_ctx);
                client_core->m_selfReference = nullptr;
            }

            void Mqtt5ClientCore::s_subscribeCompletionCallback(
                const aws_mqtt5_packet_suback_view *suback,
                int error_code,
                void *complete_ctx)
            {
                AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Subscribe completion callback triggered.");
                auto callbackData = reinterpret_cast<SubAckCallbackData *>(complete_ctx);
                AWS_ASSERT(callbackData != nullptr);
                AWS_ASSERT(callbackData->clientCore != nullptr);

                /* callback not set */
                if (callbackData->onSubscribeCompletion == NULL)
                {
                    goto on_subscribeCompletionCleanup;
                }

                {
                    std::lock_guard<std::recursive_mutex> lock(callbackData->clientCore->m_callback_lock);
                    if (callbackData->clientCore->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                    {
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "Subscribe Completion Callback: mqtt5 client is not valid, revoke the callbacks.");
                        goto on_subscribeCompletionCleanup;
                    }
                }

                {
                    std::shared_ptr<SubAckPacket> packet = nullptr;
                    if (suback != nullptr)
                    {
                        packet = std::make_shared<SubAckPacket>(*suback, callbackData->allocator);
                    }

                    if (error_code != 0)
                    {
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "SubscribeCompletion Failed with Error Code: %d(%s)",
                            error_code,
                            aws_error_debug_str(error_code));
                    }

                    callbackData->onSubscribeCompletion(error_code, packet);
                }
            on_subscribeCompletionCleanup:
                Crt::Delete(callbackData, callbackData->allocator);
            }

            void Mqtt5ClientCore::s_unsubscribeCompletionCallback(
                const aws_mqtt5_packet_unsuback_view *unsuback,
                int error_code,
                void *complete_ctx)
            {
                AWS_LOGF_INFO(AWS_LS_MQTT5_CLIENT, "Unsubscribe completion callback triggered.");
                UnSubAckCallbackData *callbackData = reinterpret_cast<UnSubAckCallbackData *>(complete_ctx);
                AWS_ASSERT(callbackData != nullptr);
                AWS_ASSERT(callbackData->clientCore != nullptr);

                /* callback not set */
                if (callbackData->onUnsubscribeCompletion == NULL)
                {
                    goto on_unsubscribeCompletionCleanup;
                }

                {
                    std::lock_guard<std::recursive_mutex> lock(callbackData->clientCore->m_callback_lock);
                    if (callbackData->clientCore->m_callbackFlag != Mqtt5ClientCore::CallbackFlag::INVOKE)
                    {
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "Unsubscribe Completion Callback: mqtt5 client is not valid, revoke the callbacks.");
                        goto on_unsubscribeCompletionCleanup;
                    }
                }

                {
                    std::shared_ptr<UnSubAckPacket> packet = nullptr;
                    if (unsuback != nullptr)
                    {
                        packet = std::make_shared<UnSubAckPacket>(*unsuback, callbackData->allocator);
                    }

                    if (error_code != 0)
                    {
                        AWS_LOGF_INFO(
                            AWS_LS_MQTT5_CLIENT,
                            "UnsubscribeCompletion Failed with Error Code: %d(%s)",
                            error_code,
                            aws_error_debug_str(error_code));
                    }

                    callbackData->onUnsubscribeCompletion(error_code, packet);
                }
            on_unsubscribeCompletionCleanup:
                Crt::Delete(callbackData, callbackData->allocator);
            }

            Mqtt5ClientCore::Mqtt5ClientCore(const Mqtt5ClientOptions &options, Allocator *allocator) noexcept
                : m_callbackFlag(CallbackFlag::INVOKE), m_client(nullptr), m_allocator(allocator)
            {
                aws_mqtt5_client_options clientOptions;

                options.initializeRawOptions(clientOptions);

                /* Setup Callbacks */
                if (options.websocketHandshakeTransform)
                {
                    this->websocketInterceptor = options.websocketHandshakeTransform;
                    clientOptions.websocket_handshake_transform = &Mqtt5ClientCore::s_onWebsocketHandshake;
                    clientOptions.websocket_handshake_transform_user_data = this;
                }

                if (options.onConnectionFailure)
                {
                    this->onConnectionFailure = options.onConnectionFailure;
                }

                if (options.onConnectionSuccess)
                {
                    this->onConnectionSuccess = options.onConnectionSuccess;
                }

                if (options.onDisconnection)
                {
                    this->onDisconnection = options.onDisconnection;
                }

                if (options.onPublishReceived)
                {
                    this->onPublishReceived = options.onPublishReceived;
                }

                if (options.onStopped)
                {
                    this->onStopped = options.onStopped;
                }

                if (options.onAttemptingConnect)
                {
                    this->onAttemptingConnect = options.onAttemptingConnect;
                }

                clientOptions.publish_received_handler_user_data = this;
                clientOptions.publish_received_handler = &Mqtt5ClientCore::s_publishReceivedCallback;

                clientOptions.lifecycle_event_handler = &Mqtt5ClientCore::s_lifeCycleEventCallback;
                clientOptions.lifecycle_event_handler_user_data = this;

                clientOptions.client_termination_handler = &Mqtt5ClientCore::s_clientTerminationCompletion;
                clientOptions.client_termination_handler_user_data = this;

                m_client = aws_mqtt5_client_new(allocator, &clientOptions);

                m_mqtt5to3AdapterOptions = Mqtt5to3AdapterOptions::NewMqtt5to3AdapterOptions(options);
            }

            Mqtt5ClientCore::~Mqtt5ClientCore() {}

            std::shared_ptr<Mqtt5ClientCore> Mqtt5ClientCore::NewMqtt5ClientCore(
                const Mqtt5ClientOptions &options,
                Allocator *allocator) noexcept
            {
                /* Copied from MqttClient.cpp: MqttClient::NewConnection) */
                /* As the constructor is private, make share would not work here. We do make_share manually. */
                Mqtt5ClientCore *toSeat =
                    reinterpret_cast<Mqtt5ClientCore *>(aws_mem_acquire(allocator, sizeof(Mqtt5ClientCore)));
                if (!toSeat)
                {
                    return nullptr;
                }

                toSeat = new (toSeat) Mqtt5ClientCore(options, allocator);

                /* Creation failed, make sure we release the allocated memory */
                if (!*toSeat)
                {
                    Crt::Delete(toSeat, allocator);
                    return nullptr;
                }

                std::shared_ptr<Mqtt5ClientCore> shared_client = std::shared_ptr<Mqtt5ClientCore>(
                    toSeat, [allocator](Mqtt5ClientCore *client) { Crt::Delete(client, allocator); });
                shared_client->m_selfReference = shared_client;
                return shared_client;
            }

            Mqtt5ClientCore::operator bool() const noexcept
            {
                return m_client != nullptr;
            }

            int Mqtt5ClientCore::LastError() const noexcept
            {
                return aws_last_error();
            }

            bool Mqtt5ClientCore::Publish(
                std::shared_ptr<PublishPacket> publishOptions,
                OnPublishCompletionHandler onPublishCompletionCallback) noexcept
            {
                if (m_client == nullptr || publishOptions == nullptr)
                {
                    return false;
                }

                aws_mqtt5_packet_publish_view publish;
                publishOptions->initializeRawOptions(publish);

                PubAckCallbackData *pubCallbackData = Aws::Crt::New<PubAckCallbackData>(m_allocator);

                pubCallbackData->clientCore = this;
                pubCallbackData->allocator = m_allocator;
                pubCallbackData->onPublishCompletion = onPublishCompletionCallback;

                aws_mqtt5_publish_completion_options options{};

                options.completion_callback = Mqtt5ClientCore::s_publishCompletionCallback;
                options.completion_user_data = pubCallbackData;

                int result = aws_mqtt5_client_publish(m_client, &publish, &options);
                if (result != AWS_OP_SUCCESS)
                {
                    Crt::Delete(pubCallbackData, pubCallbackData->allocator);
                    return false;
                }
                return result == AWS_OP_SUCCESS;
            }

            bool Mqtt5ClientCore::Subscribe(
                std::shared_ptr<SubscribePacket> subscribeOptions,
                OnSubscribeCompletionHandler onSubscribeCompletionCallback) noexcept
            {
                if (subscribeOptions == nullptr)
                {
                    return false;
                }
                /* Setup packet_subscribe */
                aws_mqtt5_packet_subscribe_view subscribe;

                subscribeOptions->initializeRawOptions(subscribe);

                /* Setup subscription Completion callback*/
                SubAckCallbackData *subCallbackData = Aws::Crt::New<SubAckCallbackData>(m_allocator);

                subCallbackData->clientCore = this;
                subCallbackData->allocator = m_allocator;
                subCallbackData->onSubscribeCompletion = onSubscribeCompletionCallback;

                aws_mqtt5_subscribe_completion_options options{};

                options.completion_callback = Mqtt5ClientCore::s_subscribeCompletionCallback;
                options.completion_user_data = subCallbackData;

                /* Subscribe to topic */
                int result = aws_mqtt5_client_subscribe(m_client, &subscribe, &options);
                if (result != AWS_OP_SUCCESS)
                {
                    Crt::Delete(subCallbackData, subCallbackData->allocator);
                    return false;
                }
                return result == AWS_OP_SUCCESS;
            }

            bool Mqtt5ClientCore::Unsubscribe(
                std::shared_ptr<UnsubscribePacket> unsubscribeOptions,
                OnUnsubscribeCompletionHandler onUnsubscribeCompletionCallback) noexcept
            {
                if (unsubscribeOptions == nullptr)
                {
                    return false;
                }

                aws_mqtt5_packet_unsubscribe_view unsubscribe;
                unsubscribeOptions->initializeRawOptions(unsubscribe);

                UnSubAckCallbackData *unSubCallbackData = Aws::Crt::New<UnSubAckCallbackData>(m_allocator);

                unSubCallbackData->clientCore = this;
                unSubCallbackData->allocator = m_allocator;
                unSubCallbackData->onUnsubscribeCompletion = onUnsubscribeCompletionCallback;

                aws_mqtt5_unsubscribe_completion_options options{};

                options.completion_callback = Mqtt5ClientCore::s_unsubscribeCompletionCallback;
                options.completion_user_data = unSubCallbackData;

                int result = aws_mqtt5_client_unsubscribe(m_client, &unsubscribe, &options);
                if (result != AWS_OP_SUCCESS)
                {
                    Crt::Delete(unSubCallbackData, unSubCallbackData->allocator);
                    return false;
                }
                return result == AWS_OP_SUCCESS;
            }

            void Mqtt5ClientCore::Close() noexcept
            {
                std::lock_guard<std::recursive_mutex> lock(m_callback_lock);
                m_callbackFlag = CallbackFlag::IGNORE;
                if (m_client != nullptr)
                {
                    aws_mqtt5_client_release(m_client);
                    m_client = nullptr;
                }
            }

            Mqtt5to3AdapterOptions::Mqtt5to3AdapterOptions() {}

            ScopedResource<Mqtt5to3AdapterOptions> Mqtt5to3AdapterOptions::NewMqtt5to3AdapterOptions(
                const Mqtt5ClientOptions &options) noexcept
            {
                Allocator *allocator = options.m_allocator;
                ScopedResource<Mqtt5to3AdapterOptions> adapterOptions = ScopedResource<Mqtt5to3AdapterOptions>(
                    Crt::New<Mqtt5to3AdapterOptions>(allocator),
                    [allocator](Mqtt5to3AdapterOptions *options) { Crt::Delete(options, allocator); });
                adapterOptions->m_mqtt3Options.allocator = options.m_allocator;
                adapterOptions->m_hostname = options.m_hostName;
                adapterOptions->m_mqtt3Options.hostName = adapterOptions->m_hostname.c_str();
                adapterOptions->m_mqtt3Options.port = options.m_port;
                adapterOptions->m_mqtt3Options.socketOptions = options.m_socketOptions;
                if (options.m_proxyOptions.has_value())
                    adapterOptions->m_proxyOptions = options.m_proxyOptions.value();
                if (options.m_tlsConnectionOptions.has_value())
                {
                    adapterOptions->m_mqtt3Options.tlsConnectionOptions = options.m_tlsConnectionOptions.value();
                    adapterOptions->m_mqtt3Options.useTls = true;
                }
                if (options.websocketHandshakeTransform)
                {
                    adapterOptions->m_mqtt3Options.useWebsocket = true;
                    adapterOptions->m_websocketHandshakeTransform = options.websocketHandshakeTransform;

                    auto signerTransform = [&adapterOptions](
                                               std::shared_ptr<Crt::Http::HttpRequest> req,
                                               const Crt::Mqtt::OnWebSocketHandshakeInterceptComplete &onComplete)
                    { adapterOptions->m_websocketHandshakeTransform(std::move(req), onComplete); };
                    adapterOptions->m_webSocketInterceptor = std::move(signerTransform);
                }
                else
                {
                    adapterOptions->m_mqtt3Options.useWebsocket = false;
                }
                return adapterOptions;
            }
        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
/*! \endcond */
