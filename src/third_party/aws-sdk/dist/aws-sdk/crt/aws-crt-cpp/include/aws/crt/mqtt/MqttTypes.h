#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Types.h>
#include <aws/crt/io/SocketOptions.h>
#include <aws/crt/io/TlsOptions.h>

#include <aws/mqtt/client.h>
#include <aws/mqtt/v5/mqtt5_client.h>

#include <functional>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt
        {
            class MqttConnection;

            /**
             * Options required to create an MqttConnection.
             */
            struct MqttConnectionOptions
            {
                const char *hostName = nullptr;
                uint32_t port = 0;
                Io::SocketOptions socketOptions;
                Crt::Io::TlsContext tlsContext;
                Crt::Io::TlsConnectionOptions tlsConnectionOptions;
                bool useWebsocket = false;
                bool useTls = false;
                Allocator *allocator = nullptr;
            };

            /**
             * Invoked upon receipt of a Publish message on a subscribed topic.
             *
             * @param connection The connection object.
             * @param topic The information channel to which the payload data was published.
             * @param payload The payload data.
             * @param dup DUP flag. If true, this might be re-delivery of an earlier attempt to send the message.
             * @param qos Quality of Service used to deliver the message.
             * @param retain Retain flag. If true, the message was sent as a result of a new subscription being made by
             * the client.
             */
            using OnMessageReceivedHandler = std::function<void(
                MqttConnection &connection,
                const String &topic,
                const ByteBuf &payload,
                bool dup,
                QOS qos,
                bool retain)>;

            /**
             * Invoked when a suback message is received.
             *
             * @param connection The connection object.
             * @param packetId Packet ID of the corresponding subscribe request.
             * @param topic The information channel to which the payload data was published.
             * @param qos Quality of Service used to deliver the message.
             * @param errorCode Indicating if an error occurred.
             */
            using OnSubAckHandler = std::function<
                void(MqttConnection &connection, uint16_t packetId, const String &topic, QOS qos, int errorCode)>;

            /**
             * Invoked when a suback message for multiple topics is received.
             *
             * @param connection The connection object.
             * @param packetId Packet ID of the corresponding subscribe request.
             * @param topics The information channels to which the payload data was published.
             * @param qos Quality of Service used to deliver the message.
             * @param errorCode Indicating if an error occurred.
             */
            using OnMultiSubAckHandler = std::function<void(
                MqttConnection &connection,
                uint16_t packetId,
                const Vector<String> &topics,
                QOS qos,
                int errorCode)>;

            /**
             * Invoked when an operation completes.
             *
             * For QoS 0, this is when the packet is passed to the tls layer. For QoS 1 (and 2, in theory) this is when
             * the final ACK packet is received from the server.
             *
             * @param connection The connection object.
             * @param packetId Packet ID of the corresponding subscribe request.
             * @param errorCode Indicating if an error occurred.
             */
            using OnOperationCompleteHandler =
                std::function<void(MqttConnection &connection, uint16_t packetId, int errorCode)>;

            /**
             * Simple statistics about the current state of the client's queue of operations.
             */
            struct AWS_CRT_CPP_API MqttConnectionOperationStatistics
            {
                /*
                 * Total number of operations submitted to the connection that have not yet been completed. Unacked
                 * operations are a subset of this.
                 */
                uint64_t incompleteOperationCount;

                /*
                 * Total packet size of operations submitted to the connection that have not yet been completed. Unacked
                 * operations are a subset of this.
                 */
                uint64_t incompleteOperationSize;

                /*
                 * Total number of operations that have been sent to the server and are waiting for a corresponding ACK
                 * before they can be completed.
                 */
                uint64_t unackedOperationCount;

                /*
                 * Total packet size of operations that have been sent to the server and are waiting for a corresponding
                 * ACK before they can be completed.
                 */
                uint64_t unackedOperationSize;
            };
        } // namespace Mqtt
    } // namespace Crt
} // namespace Aws
