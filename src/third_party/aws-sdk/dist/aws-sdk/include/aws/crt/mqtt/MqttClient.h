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
#include <aws/crt/mqtt/MqttConnection.h>

#include <aws/mqtt/client.h>
#include <aws/mqtt/v5/mqtt5_client.h>

#include <atomic>
#include <functional>
#include <memory>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            class ClientBootstrap;
        }

        namespace Http
        {
            class HttpRequest;
        }

        namespace Mqtt5
        {
            class Mqtt5ClientCore;
        }

        namespace Mqtt
        {
            /**
             * An MQTT client. This is a move-only type. Unless otherwise specified,
             * all function arguments need only to live through the duration of the
             * function call.
             */
            class AWS_CRT_CPP_API MqttClient final
            {
              public:
                /**
                 * Initialize an MqttClient using bootstrap and allocator
                 */
                MqttClient(Io::ClientBootstrap &bootstrap, Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * Initialize an MqttClient using a allocator and the default ClientBootstrap
                 *
                 * For more information on the default ClientBootstrap see
                 * Aws::Crt::ApiHandle::GetOrCreateStaticDefaultClientBootstrap
                 */
                MqttClient(Allocator *allocator = ApiAllocator()) noexcept;

                ~MqttClient();
                MqttClient(const MqttClient &) = delete;
                MqttClient(MqttClient &&) noexcept;
                MqttClient &operator=(const MqttClient &) = delete;
                MqttClient &operator=(MqttClient &&) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept;

                /**
                 * Create a new connection object using TLS from the client. The client must outlive
                 * all of its connection instances.
                 *
                 * @param hostName endpoint to connect to
                 * @param port port to connect to
                 * @param socketOptions socket options to use when establishing the connection
                 * @param tlsContext tls context to use with the connection
                 * @param useWebsocket should the connection use websockets or should it use direct mqtt?
                 *
                 * @return a new connection object.  Connect() will still need to be called after all further
                 * configuration is finished.
                 */
                std::shared_ptr<MqttConnection> NewConnection(
                    const char *hostName,
                    uint32_t port,
                    const Io::SocketOptions &socketOptions,
                    const Crt::Io::TlsContext &tlsContext,
                    bool useWebsocket = false) noexcept;

                /**
                 * Create a new connection object over plain text from the client. The client must outlive
                 * all of its connection instances.
                 * @param hostName endpoint to connect to
                 * @param port port to connect to
                 * @param socketOptions socket options to use when establishing the connection
                 * @param useWebsocket should the connection use websockets or should it use direct mqtt?
                 *
                 * @return a new connection object.  Connect() will still need to be called after all further
                 * configuration is finished.
                 */
                std::shared_ptr<MqttConnection> NewConnection(
                    const char *hostName,
                    uint32_t port,
                    const Io::SocketOptions &socketOptions,
                    bool useWebsocket = false) noexcept;

              private:
                aws_mqtt_client *m_client;
            };
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
