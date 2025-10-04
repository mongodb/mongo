/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/mqtt/MqttClient.h>

#include <aws/crt/Api.h>
#include <aws/crt/StlAllocator.h>
#include <aws/crt/http/HttpProxyStrategy.h>
#include <aws/crt/http/HttpRequestResponse.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/crt/mqtt/MqttConnection.h>

#include <utility>

#define AWS_MQTT_MAX_TOPIC_LENGTH 65535

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt
        {
            MqttClient::MqttClient(Io::ClientBootstrap &bootstrap, Allocator *allocator) noexcept
                : m_client(aws_mqtt_client_new(allocator, bootstrap.GetUnderlyingHandle()))
            {
            }

            MqttClient::MqttClient(Allocator *allocator) noexcept
                : m_client(aws_mqtt_client_new(
                      allocator,
                      Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap()->GetUnderlyingHandle()))
            {
            }

            MqttClient::~MqttClient()
            {
                aws_mqtt_client_release(m_client);
                m_client = nullptr;
            }

            MqttClient::MqttClient(MqttClient &&toMove) noexcept : m_client(toMove.m_client)
            {
                toMove.m_client = nullptr;
            }

            MqttClient &MqttClient::operator=(MqttClient &&toMove) noexcept
            {
                if (&toMove != this)
                {
                    m_client = toMove.m_client;
                    toMove.m_client = nullptr;
                }

                return *this;
            }

            MqttClient::operator bool() const noexcept
            {
                return m_client != nullptr;
            }

            int MqttClient::LastError() const noexcept
            {
                return aws_last_error();
            }

            std::shared_ptr<MqttConnection> MqttClient::NewConnection(
                const char *hostName,
                uint32_t port,
                const Io::SocketOptions &socketOptions,
                const Crt::Io::TlsContext &tlsContext,
                bool useWebsocket) noexcept
            {
                if (!tlsContext)
                {
                    AWS_LOGF_ERROR(
                        AWS_LS_MQTT_CLIENT,
                        "id=%p Trying to call MqttClient::NewConnection using an invalid TlsContext.",
                        (void *)m_client);
                    aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                    return nullptr;
                }

                MqttConnectionOptions connectionOptions;
                connectionOptions.hostName = hostName;
                connectionOptions.port = port;
                connectionOptions.socketOptions = socketOptions;
                connectionOptions.tlsContext = tlsContext;
                connectionOptions.tlsConnectionOptions = tlsContext.NewConnectionOptions();
                connectionOptions.useWebsocket = useWebsocket;
                connectionOptions.useTls = true;
                connectionOptions.allocator = m_client->allocator;

                return MqttConnection::s_CreateMqttConnection(m_client, std::move(connectionOptions));
            }

            std::shared_ptr<MqttConnection> MqttClient::NewConnection(
                const char *hostName,
                uint32_t port,
                const Io::SocketOptions &socketOptions,
                bool useWebsocket) noexcept

            {
                MqttConnectionOptions connectionOptions;
                connectionOptions.hostName = hostName;
                connectionOptions.port = port;
                connectionOptions.socketOptions = socketOptions;
                connectionOptions.useWebsocket = useWebsocket;
                connectionOptions.useTls = false;
                connectionOptions.allocator = m_client->allocator;

                return MqttConnection::s_CreateMqttConnection(m_client, std::move(connectionOptions));
            }
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
