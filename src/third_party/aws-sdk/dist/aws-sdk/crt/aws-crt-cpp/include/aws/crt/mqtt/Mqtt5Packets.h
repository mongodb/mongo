#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>
#include <aws/crt/mqtt/Mqtt5Types.h>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {

            /**
             * Data model for MQTT5 user properties.
             *
             * A user property is a name-value pair of utf-8 strings that can be added to mqtt5 packets.
             */
            class AWS_CRT_CPP_API UserProperty
            {
              public:
                UserProperty(Crt::String key, Crt::String value) noexcept;

                const Crt::String &getName() const noexcept { return m_name; };
                const Crt::String &getValue() const noexcept { return m_value; }

                ~UserProperty() noexcept;
                UserProperty(const UserProperty &toCopy) noexcept;
                UserProperty(UserProperty &&toMove) noexcept;
                UserProperty &operator=(const UserProperty &toCopy) noexcept;
                UserProperty &operator=(UserProperty &&toMove) noexcept;

              private:
                Crt::String m_name;
                Crt::String m_value;
            };

            class AWS_CRT_CPP_API IPacket
            {
              public:
                virtual PacketType getType() = 0;
            };

            /**
             * Data model of an [MQTT5
             * PUBLISH](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901100) packet
             */
            class AWS_CRT_CPP_API PublishPacket : public IPacket
            {
              public:
                PublishPacket(
                    const aws_mqtt5_packet_publish_view &raw_options,
                    Allocator *allocator = ApiAllocator()) noexcept;
                PublishPacket(Allocator *allocator = ApiAllocator()) noexcept;
                PublishPacket(
                    Crt::String topic,
                    ByteCursor payload,
                    Mqtt5::QOS qos,
                    Allocator *allocator = ApiAllocator()) noexcept;
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_PUBLISH; };

                /**
                 * Sets the payload for the publish message.
                 *
                 * See [MQTT5 Publish
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901119)
                 *
                 * @param payload The payload for the publish message.
                 * @return The PublishPacket Object after setting the payload.
                 */
                PublishPacket &WithPayload(ByteCursor payload) noexcept;

                /**
                 * Sets the MQTT quality of service level the message should be delivered with.
                 *
                 * See [MQTT5 QoS](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901103)
                 *
                 * @param packetQOS The MQTT quality of service level the message should be delivered with.
                 * @return The PublishPacket Object after setting the QOS.
                 */
                PublishPacket &WithQOS(Mqtt5::QOS packetQOS) noexcept;

                /**
                 * Sets if this should be a retained message.
                 *
                 * See [MQTT5 Retain](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901104)
                 *
                 * @param retain if this is a retained message.
                 * @return The PublishPacket Object after setting the retain setting.
                 */
                PublishPacket &WithRetain(bool retain) noexcept;

                /**
                 * Sets the topic this message should be published to.
                 * See [MQTT5 Topic Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901107)
                 *
                 * @param topic The topic this message should be published to.
                 * @return The PublishPacket Object after setting the topic.
                 */
                PublishPacket &WithTopic(Crt::String topic) noexcept;

                /**
                 * Sets the property specifying the format of the payload data. The mqtt5 client does not enforce or use
                 * this value in a meaningful way.
                 *
                 * See [MQTT5 Payload Format
                 * Indicator](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901111)
                 *
                 * @param payloadFormat Property specifying the format of the payload data
                 * @return The PublishPacket Object after setting the payload format.
                 */
                PublishPacket &WithPayloadFormatIndicator(PayloadFormatIndicator payloadFormat) noexcept;

                /**
                 * Sets the maximum amount of time allowed to elapse for message delivery before the server
                 * should instead delete the message (relative to a recipient).
                 *
                 * See [MQTT5 Message Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901112)
                 *
                 * @param second The maximum amount of time allowed to elapse for message delivery
                 * before the server should instead delete the message (relative to a recipient).
                 * @return The PublishPacket Object after setting the message expiry interval.
                 */
                PublishPacket &WithMessageExpiryIntervalSec(uint32_t second) noexcept;

                /**
                 * Sets the opic alias to use, if possible, when encoding this packet.  Only used if the
                 * client's outbound topic aliasing mode is set to Manual.
                 *
                 * See [MQTT5 Topic Alias](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113)
                 */
                PublishPacket &WithTopicAlias(uint16_t topicAlias) noexcept;

                /**
                 * Sets the opaque topic string intended to assist with request/response implementations.  Not
                 * internally meaningful to MQTT5 or this client.
                 *
                 * See [MQTT5 Response
                 * Topic](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901114)
                 * @param responseTopic
                 * @return The PublishPacket Object after setting the response topic.
                 */
                PublishPacket &WithResponseTopic(ByteCursor responseTopic) noexcept;

                /**
                 * Sets the opaque binary data used to correlate between publish messages, as a potential method for
                 * request-response implementation.  Not internally meaningful to MQTT5.
                 *
                 * See [MQTT5 Correlation
                 * Data](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901115)
                 *
                 * @param correlationData Opaque binary data used to correlate between publish messages
                 * @return The PublishPacket Object after setting the correlation data.
                 */
                PublishPacket &WithCorrelationData(ByteCursor correlationData) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The PublishPacket Object after setting the user properties
                 */
                PublishPacket &WithUserProperties(const Vector<UserProperty> &userProperties) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The PublishPacket Object after setting the user properties
                 */
                PublishPacket &WithUserProperties(Vector<UserProperty> &&userProperties) noexcept;

                /**
                 * Put a MQTT5 user property to the back of the packet user property vector/list
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param property set of userProperty of MQTT5 user properties included with the packet.
                 * @return The PublishPacket Object after setting the user property
                 */
                PublishPacket &WithUserProperty(UserProperty &&property) noexcept;

                bool initializeRawOptions(aws_mqtt5_packet_publish_view &raw_options) noexcept;

                /**
                 * The payload of the publish message.
                 *
                 * See [MQTT5 Publish
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901119)
                 *
                 * @return The payload of the publish message.
                 */
                const ByteCursor &getPayload() const noexcept;

                /**
                 * Sent publishes - The MQTT quality of service level this message should be delivered with.
                 *
                 * Received publishes - The MQTT quality of service level this message was delivered at.
                 *
                 * See [MQTT5 QoS](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901103)
                 *
                 * @return The MQTT quality of service associated with this PUBLISH packet.
                 */
                Mqtt5::QOS getQOS() const noexcept;

                /**
                 * True if this is a retained message, false otherwise.
                 *
                 * Always set on received publishes.
                 *
                 * See [MQTT5 Retain](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901104)
                 *
                 * @return True if this is a retained message, false otherwise.
                 */
                bool getRetain() const noexcept;

                /**
                 * Sent publishes - The topic this message should be published to.
                 *
                 * Received publishes - The topic this message was published to.
                 *
                 * See [MQTT5 Topic Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901107)
                 * @return The topic associated with this PUBLISH packet.
                 */
                const Crt::String &getTopic() const noexcept;

                /**
                 * Property specifying the format of the payload data. The mqtt5 client does not enforce or use this
                 * value in a meaningful way.
                 *
                 * See [MQTT5 Payload Format
                 * Indicator](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901111)
                 *
                 * @return Property specifying the format of the payload data.
                 */
                const Crt::Optional<PayloadFormatIndicator> &getPayloadFormatIndicator() const noexcept;

                /**
                 * Sent publishes - indicates the maximum amount of time allowed to elapse for message delivery before
                 * the server should instead delete the message (relative to a recipient).
                 *
                 * Received publishes - indicates the remaining amount of time (from the server's perspective) before
                 * the message would have been deleted relative to the subscribing client.
                 *
                 * If left null, indicates no expiration timeout.
                 *
                 * See [MQTT5 Message Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901112)
                 *
                 * @return The message expiry interval associated with this PUBLISH packet.
                 */
                const Crt::Optional<uint32_t> &getMessageExpiryIntervalSec() const noexcept;

                /**
                 * Sent publishes - Topic alias to use, if possible, when encoding this packet.  Only used if the
                 * client's outbound topic aliasing mode is set to Manual.
                 *
                 * Received publishes - topic alias used by the server when transmitting the publish to the client.
                 *
                 * See [MQTT5 Topic Alias](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113)
                 *
                 * @return the topic alias, if any, associated with this PUBLISH packet
                 */
                const Crt::Optional<uint16_t> &getTopicAlias() const noexcept;

                /**
                 * Opaque topic string intended to assist with request/response implementations.  Not internally
                 * meaningful to MQTT5 or this client.
                 *
                 * See [MQTT5 Response
                 * Topic](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901114)
                 *
                 * @return ByteCursor to topic string intended to assist with request/response implementations.
                 */
                const Crt::Optional<ByteCursor> &getResponseTopic() const noexcept;

                /**
                 * Opaque binary data used to correlate between publish messages, as a potential method for
                 * request-response implementation.  Not internally meaningful to MQTT5.
                 *
                 * See [MQTT5 Correlation
                 * Data](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901115)
                 *
                 * @return ByteCursor to opaque binary data used to correlate between publish messages.
                 */
                const Crt::Optional<ByteCursor> &getCorrelationData() const noexcept;

                /**
                 * Sent publishes - ignored
                 *
                 * Received publishes - the subscription identifiers of all the subscriptions this message matched.
                 *
                 * See [MQTT5 Subscription
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901117)
                 *
                 * @return the subscription identifiers of all the subscriptions this message matched.
                 */
                const Crt::Vector<uint32_t> &getSubscriptionIdentifiers() const noexcept;

                /**
                 * Property specifying the content type of the payload.  Not internally meaningful to MQTT5.
                 *
                 * See [MQTT5 Content Type](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901118)
                 *
                 * @return ByteCursor to opaque binary data to the content type of the payload.
                 */
                const Crt::Optional<ByteCursor> &getContentType() const noexcept;

                /**
                 * List of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                virtual ~PublishPacket();
                PublishPacket(const PublishPacket &) = delete;
                PublishPacket(PublishPacket &&) noexcept = delete;
                PublishPacket &operator=(const PublishPacket &) = delete;
                PublishPacket &operator=(PublishPacket &&) noexcept = delete;

              private:
                Allocator *m_allocator;

                /**
                 * The payload of the publish message.
                 *
                 * See [MQTT5 Publish
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901119)
                 */
                ByteCursor m_payload;

                /**
                 * Sent publishes - The MQTT quality of service level this message should be delivered with.
                 *
                 * Received publishes - The MQTT quality of service level this message was delivered at.
                 *
                 * See [MQTT5 QoS](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901103)
                 */
                Mqtt5::QOS m_qos;

                /**
                 * True if this is a retained message, false otherwise.
                 *
                 * Always set on received publishes, default to false
                 *
                 * See [MQTT5 Retain](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901104)
                 */
                bool m_retain;

                /**
                 * Sent publishes - The topic this message should be published to.
                 *
                 * Received publishes - The topic this message was published to.
                 *
                 * See [MQTT5 Topic Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901107)
                 */
                Crt::String m_topicName;

                /**
                 * Property specifying the format of the payload data.  The mqtt5 client does not enforce or use this
                 * value in a meaningful way.
                 *
                 * See [MQTT5 Payload Format
                 * Indicator](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901111)
                 */
                Crt::Optional<PayloadFormatIndicator> m_payloadFormatIndicator;

                /**
                 * Sent publishes - indicates the maximum amount of time allowed to elapse for message delivery before
                 * the server should instead delete the message (relative to a recipient).
                 *
                 * Received publishes - indicates the remaining amount of time (from the server's perspective) before
                 * the message would have been deleted relative to the subscribing client.
                 *
                 * If left undefined, indicates no expiration timeout.
                 *
                 * See [MQTT5 Message Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901112)
                 */
                Crt::Optional<uint32_t> m_messageExpiryIntervalSec;

                /**
                 * Sent publishes - Topic alias to use, if possible, when encoding this packet.  Only used if the
                 * client's outbound topic aliasing mode is set to Manual.
                 *
                 * Received publishes - topic alias used by the server when transmitting the publish to the client.
                 *
                 * See [MQTT5 Topic Alias](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901113)
                 */
                Crt::Optional<uint16_t> m_topicAlias;

                /**
                 * Opaque topic string intended to assist with request/response implementations.  Not internally
                 * meaningful to MQTT5 or this client.
                 *
                 * See [MQTT5 Response
                 * Topic](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901114)
                 */
                Crt::Optional<ByteCursor> m_responseTopic;

                /**
                 * Opaque binary data used to correlate between publish messages, as a potential method for
                 * request-response implementation.  Not internally meaningful to MQTT5.
                 *
                 * See [MQTT5 Correlation
                 * Data](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901115)
                 */
                Crt::Optional<ByteCursor> m_correlationData;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                ///////////////////////////////////////////////////////////////////////////
                // The following parameters are ignored when building publish operations */
                ///////////////////////////////////////////////////////////////////////////

                /**
                 * Sent publishes - ignored
                 *
                 * Received publishes - the subscription identifiers of all the subscriptions this message matched.
                 *
                 * See [MQTT5 Subscription
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901117)
                 */
                Crt::Vector<uint32_t> m_subscriptionIdentifiers;

                /**
                 * Property specifying the content type of the payload.  Not internally meaningful to MQTT5.
                 *
                 * See [MQTT5 Content Type](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901118)
                 */
                Crt::Optional<ByteCursor> m_contentType;

                ///////////////////////////////////////////////////////////////////////////
                // Underlying data storage for internal use
                ///////////////////////////////////////////////////////////////////////////
                ByteBuf m_payloadStorage;
                ByteBuf m_contentTypeStorage;
                ByteBuf m_correlationDataStorage;
                Crt::String m_responseTopicString;
                struct aws_mqtt5_user_property *m_userPropertiesStorage;
            };

            /**
             * Mqtt behavior settings that are dynamically negotiated as part of the CONNECT/CONNACK exchange.
             *
             * While you can infer all of these values from a combination of
             *   (1) defaults as specified in the mqtt5 spec
             *   (2) your CONNECT settings
             *   (3) the CONNACK from the broker
             *
             * the client instead does the combining for you and emits a NegotiatedSettings object with final,
             * authoritative values.
             *
             * Negotiated settings are communicated with every successful connection establishment.
             */
            class AWS_CRT_CPP_API NegotiatedSettings
            {
              public:
                NegotiatedSettings(
                    const aws_mqtt5_negotiated_settings &negotiated_settings,

                    Allocator *allocator = ApiAllocator()) noexcept;

                /**
                 * @return The maximum QoS allowed for publishes on this connection instance
                 */
                Mqtt5::QOS getMaximumQOS() const noexcept;

                /**
                 * @return The amount of time in seconds the server will retain the MQTT session after a disconnect.
                 */
                uint32_t getSessionExpiryIntervalSec() const noexcept;

                /**
                 * @return The number of in-flight QoS 1 and QoS 2 publications the server is willing to process
                 * concurrently.
                 */
                uint16_t getReceiveMaximumFromServer() const noexcept;

                /**
                 * @deprecated the function is deprecated, please use
                 * `NegotiatedSettings::getMaximumPacketSizeToServer()`
                 *
                 * @return The maximum packet size the server is willing to accept.
                 */
                uint32_t getMaximumPacketSizeBytes() const noexcept;

                /**
                 * @return The maximum packet size the server is willing to accept.
                 */
                uint32_t getMaximumPacketSizeToServer() const noexcept;

                /**
                 * @return returns the maximum allowed topic alias value on publishes sent from client to server
                 */
                uint16_t getTopicAliasMaximumToServer() const noexcept;

                /**
                 * @return returns the maximum allowed topic alias value on publishes sent from server to client
                 */
                uint16_t getTopicAliasMaximumToClient() const noexcept;

                /**
                 * The maximum amount of time in seconds between client packets. The client should use PINGREQs to
                 * ensure this limit is not breached.  The server will disconnect the client for inactivity if no MQTT
                 * packet is received in a time interval equal to 1.5 x this value.
                 *
                 * @return The maximum amount of time in seconds between client packets.
                 */
                uint16_t getServerKeepAliveSec() const noexcept;

                /**
                 * @deprecated The function is deprecated, please use `NegotiatedSettings::getServerKeepAliveSec()`
                 *
                 * The maximum amount of time in seconds between client packets. The client should use PINGREQs to
                 * ensure this limit is not breached.  The server will disconnect the client for inactivity if no MQTT
                 * packet is received in a time interval equal to 1.5 x this value.
                 *
                 * @return The maximum amount of time in seconds between client packets.
                 */
                uint16_t getServerKeepAlive() const noexcept;

                /**
                 * @return Whether the server supports retained messages.
                 */
                bool getRetainAvailable() const noexcept;

                /**
                 * @return Whether the server supports wildcard subscriptions.
                 */
                bool getWildcardSubscriptionsAvailable() const noexcept;

                /**
                 * @return Whether the server supports subscription identifiers
                 */
                bool getSubscriptionIdentifiersAvailable() const noexcept;

                /**
                 * @return Whether the server supports shared subscriptions
                 */
                bool getSharedSubscriptionsAvailable() const noexcept;

                /**
                 * @return Whether the client has rejoined an existing session.
                 */
                bool getRejoinedSession() const noexcept;

                /**
                 * The final client id in use by the newly-established connection.  This will be the configured client
                 * id if one was given in the configuration, otherwise, if no client id was specified, this will be the
                 * client id assigned by the server.  Reconnection attempts will always use the auto-assigned client id,
                 * allowing for auto-assigned session resumption.
                 *
                 * @return The final client id in use by the newly-established connection
                 */
                const Crt::String &getClientId() const noexcept;

                virtual ~NegotiatedSettings() {};
                NegotiatedSettings(const NegotiatedSettings &) = delete;
                NegotiatedSettings(NegotiatedSettings &&) noexcept = delete;
                NegotiatedSettings &operator=(const NegotiatedSettings &) = delete;
                NegotiatedSettings &operator=(NegotiatedSettings &&) noexcept = delete;

              private:
                /**
                 * The maximum QoS allowed for publishes on this connection instance
                 */
                Mqtt5::QOS m_maximumQOS;

                /**
                 * The amount of time in seconds the server will retain the MQTT session after a disconnect.
                 */
                uint32_t m_sessionExpiryIntervalSec;

                /**
                 * The number of in-flight QoS 1 and QoS2 publications the server is willing to process concurrently.
                 */
                uint16_t m_receiveMaximumFromServer;

                /**
                 * The maximum packet size the server is willing to accept.
                 */
                uint32_t m_maximumPacketSizeBytes;

                /**
                 * the maximum allowed topic alias value on publishes sent from client to server
                 */
                uint16_t m_topicAliasMaximumToServer;

                /**
                 * the maximum allowed topic alias value on publishes sent from server to client
                 */
                uint16_t m_topicAliasMaximumToClient;

                /**
                 * The maximum amount of time in seconds between client packets.  The client should use PINGREQs to
                 * ensure this limit is not breached.  The server will disconnect the client for inactivity if no MQTT
                 * packet is received in a time interval equal to 1.5 x this value.
                 */
                uint16_t m_serverKeepAliveSec;

                /**
                 * Whether the server supports retained messages.
                 */
                bool m_retainAvailable;

                /**
                 * Whether the server supports wildcard subscriptions.
                 */
                bool m_wildcardSubscriptionsAvailable;

                /**
                 * Whether the server supports subscription identifiers
                 */
                bool m_subscriptionIdentifiersAvailable;

                /**
                 * Whether the server supports shared subscriptions
                 */
                bool m_sharedSubscriptionsAvailable;

                /**
                 * Whether the client has rejoined an existing session.
                 */
                bool m_rejoinedSession;

                /**
                 * The final client id in use by the newly-established connection.  This will be the configured client
                 * id if one was given in the configuration, otherwise, if no client id was specified, this will be the
                 * client id assigned by the server.  Reconnection attempts will always use the auto-assigned client id,
                 * allowing for auto-assigned session resumption.
                 */
                Crt::String m_clientId;
            };

            /**
             * Data model of an [MQTT5
             * CONNECT](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901033) packet.
             */
            class AWS_CRT_CPP_API ConnectPacket : public IPacket
            {
              public:
                /* Default constructor */
                ConnectPacket(Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_CONNECT; };

                /**
                 * Sets the maximum time interval, in seconds, that is permitted to elapse between the point at which
                 * the client finishes transmitting one MQTT packet and the point it starts sending the next.  The
                 * client will use PINGREQ packets to maintain this property.
                 *
                 * If the responding CONNACK contains a keep alive property value, then that is the negotiated keep
                 * alive value. Otherwise, the keep alive sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Keep Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901045)
                 *
                 * NOTE: The keepAliveIntervalSeconds HAS to be larger than the pingTimeoutMs time set in the
                 * Mqtt5ClientOptions.
                 *
                 * @param keepAliveInteralSeconds the maximum time interval, in seconds, that is permitted to elapse
                 * between the point at which the client finishes transmitting one MQTT packet and the point it starts
                 * sending the next.
                 * @return The ConnectPacket Object after setting the keep alive interval.
                 */
                ConnectPacket &WithKeepAliveIntervalSec(uint16_t keepAliveInteralSeconds) noexcept;

                /**
                 * Sets the unique string identifying the client to the server.  Used to restore session state between
                 * connections.
                 *
                 * If left empty, the broker will auto-assign a unique client id.  When reconnecting, the mqtt5 client
                 * will always use the auto-assigned client id.
                 *
                 * See [MQTT5 Client
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901059)
                 *
                 * @param clientId A unique string identifying the client to the server.
                 * @return The ConnectPacket Object after setting the client ID.
                 */
                ConnectPacket &WithClientId(Crt::String clientId) noexcept;

                /**
                 * Sets the string value that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 User Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901071)
                 *
                 * @param username The string value that the server may use for client authentication and authorization.
                 * @return The ConnectPacket Object after setting the username.
                 */
                ConnectPacket &WithUserName(Crt::String username) noexcept;

                /**
                 * Sets the opaque binary data that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 Password](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901072)
                 *
                 * @param password Opaque binary data that the server may use for client authentication and
                 * authorization.
                 * @return The ConnectPacket Object after setting the password.
                 */
                ConnectPacket &WithPassword(ByteCursor password) noexcept;

                /**
                 * Sets the time interval, in seconds, that the client requests the server to persist this connection's
                 * MQTT session state for.  Has no meaning if the client has not been configured to rejoin sessions.
                 * Must be non-zero in order to successfully rejoin a session.
                 *
                 * If the responding CONNACK contains a session expiry property value, then that is the negotiated
                 * session expiry value.  Otherwise, the session expiry sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901048)
                 *
                 * @param sessionExpiryIntervalSeconds A time interval, in seconds, that the client requests the server
                 * to persist this connection's MQTT session state for.
                 * @return The ConnectPacket Object after setting the session expiry interval.
                 */
                ConnectPacket &WithSessionExpiryIntervalSec(uint32_t sessionExpiryIntervalSeconds) noexcept;

                /**
                 * Sets whether requests that the server send response information in the subsequent CONNACK.  This
                 * response information may be used to set up request-response implementations over MQTT, but doing so
                 * is outside the scope of the MQTT5 spec and client.
                 *
                 * See [MQTT5 Request Response
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901052)
                 *
                 * @param requestResponseInformation If true, requests that the server send response information in the
                 * subsequent CONNACK.
                 * @return The ConnectPacket Object after setting the request response information.
                 */
                ConnectPacket &WithRequestResponseInformation(bool requestResponseInformation) noexcept;

                /**
                 * Sets whether requests that the server send additional diagnostic information (via response string or
                 * user properties) in DISCONNECT or CONNACK packets from the server.
                 *
                 * See [MQTT5 Request Problem
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901053)
                 *
                 * @param requestProblemInformation If true, requests that the server send additional diagnostic
                 * information (via response string or user properties) in DISCONNECT or CONNACK packets from the
                 * server.
                 * @return The ConnectPacket Object after setting the request problem information.
                 */
                ConnectPacket &WithRequestProblemInformation(bool requestProblemInformation) noexcept;

                /**
                 * Sets the maximum number of in-flight QoS 1 and 2 messages the client is willing to handle.  If
                 * omitted, then no limit is requested.
                 *
                 * See [MQTT5 Receive
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901049)
                 *
                 * @param receiveMaximum The maximum number of in-flight QoS 1 and 2 messages the client is willing to
                 * handle.
                 * @return The ConnectPacket Object after setting the receive maximum.
                 */
                ConnectPacket &WithReceiveMaximum(uint16_t receiveMaximum) noexcept;

                /**
                 * Sets the maximum packet size the client is willing to handle.  If
                 * omitted, then no limit beyond the natural limits of MQTT packet size is requested.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901050)
                 *
                 * @param maximumPacketSizeBytes The maximum packet size the client is willing to handle
                 * @return The ConnectPacket Object after setting the maximum packet size.
                 */
                ConnectPacket &WithMaximumPacketSizeBytes(uint32_t maximumPacketSizeBytes) noexcept;

                /**
                 * Sets the time interval, in seconds, that the server should wait (for a session reconnection) before
                 * sending the will message associated with the connection's session.  If omitted, the server
                 * will send the will when the associated session is destroyed.  If the session is destroyed before a
                 * will delay interval has elapsed, then the will must be sent at the time of session destruction.
                 *
                 * See [MQTT5 Will Delay
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901062)
                 *
                 * @param willDelayIntervalSeconds A time interval, in seconds, that the server should wait (for a
                 * session reconnection) before sending the will message associated with the connection's session.
                 * @return The ConnectPacket Object after setting the will message delay interval.
                 */
                ConnectPacket &WithWillDelayIntervalSec(uint32_t willDelayIntervalSeconds) noexcept;

                /**
                 * Sets the definition of a message to be published when the connection's session is destroyed by the
                 * server or when the will delay interval has elapsed, whichever comes first.  If null, then nothing
                 * will be sent.
                 *
                 * See [MQTT5 Will](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901040)
                 *
                 * @param will The message to be published when the connection's session is destroyed by the server or
                 * when the will delay interval has elapsed, whichever comes first.
                 * @return The ConnectPacket Object after setting the will message.
                 */
                ConnectPacket &WithWill(std::shared_ptr<PublishPacket> will) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901054)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The ConnectPacket Object after setting the user properties.
                 */
                ConnectPacket &WithUserProperties(const Vector<UserProperty> &userProperties) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901054)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The ConnectPacket Object after setting the user properties.
                 */
                ConnectPacket &WithUserProperties(Vector<UserProperty> &&userProperties) noexcept;

                /**
                 * Put a MQTT5 user property to the back of the packet user property vector/list
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param property set of userProperty of MQTT5 user properties included with the packet.
                 * @return The ConnectPacket Object after setting the user property
                 */
                ConnectPacket &WithUserProperty(UserProperty &&property) noexcept;

                /********************************************
                 * Access Functions
                 ********************************************/

                /**
                 * The maximum time interval, in seconds, that is permitted to elapse between the point at which the
                 * client finishes transmitting one MQTT packet and the point it starts sending the next.  The client
                 * will use PINGREQ packets to maintain this property.
                 *
                 * If the responding CONNACK contains a keep alive property value, then that is the negotiated keep
                 * alive value. Otherwise, the keep alive sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Keep Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901045)
                 *
                 * @return The maximum time interval, in seconds, that is permitted to elapse between the point at which
                 * the client finishes transmitting one MQTT packet and the point it starts sending the next.
                 */
                uint16_t getKeepAliveIntervalSec() const noexcept;

                /**
                 * A unique string identifying the client to the server.  Used to restore session state between
                 * connections.
                 *
                 * If left empty, the broker will auto-assign a unique client id.  When reconnecting, the mqtt5 client
                 * will always use the auto-assigned client id.
                 *
                 * See [MQTT5 Client
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901059)
                 *
                 * @return A unique string identifying the client to the server.
                 */
                const Crt::String &getClientId() const noexcept;

                /**
                 * A string value that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 User Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901071)
                 *
                 * @return A string value that the server may use for client authentication and authorization.
                 */
                const Crt::Optional<Crt::String> &getUsername() const noexcept;

                /**
                 * Opaque binary data that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 Password](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901072)
                 *
                 * @return Opaque binary data that the server may use for client authentication and authorization.
                 */
                const Crt::Optional<Crt::ByteCursor> &getPassword() const noexcept;

                /**
                 * A time interval, in seconds, that the client requests the server to persist this connection's MQTT
                 * session state for.  Has no meaning if the client has not been configured to rejoin sessions.  Must be
                 * non-zero in order to successfully rejoin a session.
                 *
                 * If the responding CONNACK contains a session expiry property value, then that is the negotiated
                 * session expiry value.  Otherwise, the session expiry sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901048)
                 *
                 * @return A time interval, in seconds, that the client requests the server to persist this connection's
                 * MQTT session state for.
                 */
                const Crt::Optional<uint32_t> &getSessionExpiryIntervalSec() const noexcept;

                /**
                 * If true, requests that the server send response information in the subsequent CONNACK.  This response
                 * information may be used to set up request-response implementations over MQTT, but doing so is outside
                 * the scope of the MQTT5 spec and client.
                 *
                 * See [MQTT5 Request Response
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901052)
                 *
                 * @return If true, requests that the server send response information in the subsequent CONNACK.
                 */
                const Crt::Optional<bool> &getRequestResponseInformation() const noexcept;

                /**
                 * If true, requests that the server send additional diagnostic information (via response string or
                 * user properties) in DISCONNECT or CONNACK packets from the server.
                 *
                 * See [MQTT5 Request Problem
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901053)
                 *
                 * @return If true, requests that the server send additional diagnostic information (via response string
                 * or user properties) in DISCONNECT or CONNACK packets from the server.
                 */
                const Crt::Optional<bool> &getRequestProblemInformation() const noexcept;

                /**
                 * Notifies the server of the maximum number of in-flight QoS 1 and 2 messages the client is willing to
                 * handle.  If omitted or null, then no limit is requested.
                 *
                 * See [MQTT5 Receive
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901049)
                 *
                 * @return The maximum number of in-flight QoS 1 and 2 messages the client is willing to handle.
                 */
                const Crt::Optional<uint16_t> &getReceiveMaximum() const noexcept;

                /**
                 * @deprecated The function is deprecated, please use `ConnectPacket::getMaximumPacketSizeToServer()`
                 *
                 * Notifies the server of the maximum packet size the client is willing to handle.  If
                 * omitted or null, then no limit beyond the natural limits of MQTT packet size is requested.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901050)
                 *
                 * @return The maximum packet size the client is willing to handle
                 */
                const Crt::Optional<uint32_t> &getMaximumPacketSizeBytes() const noexcept;

                /**
                 * Notifies the server of the maximum packet size the client is willing to handle.  If
                 * omitted or null, then no limit beyond the natural limits of MQTT packet size is requested.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901050)
                 *
                 * @return The maximum packet size the client is willing to handle
                 */
                const Crt::Optional<uint32_t> &getMaximumPacketSizeToServer() const noexcept;

                /**
                 * A time interval, in seconds, that the server should wait (for a session reconnection) before sending
                 * the will message associated with the connection's session.  If omitted or null, the server will send
                 * the will when the associated session is destroyed.  If the session is destroyed before a will delay
                 * interval has elapsed, then the will must be sent at the time of session destruction.
                 *
                 * See [MQTT5 Will Delay
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901062)
                 *
                 * @return A time interval, in seconds, that the server should wait (for a session reconnection) before
                 * sending the will message associated with the connection's session.
                 */
                const Crt::Optional<uint32_t> &getWillDelayIntervalSec() const noexcept;

                /**
                 * The definition of a message to be published when the connection's session is destroyed by the server
                 * or when the will delay interval has elapsed, whichever comes first.  If null, then nothing will be
                 * sent.
                 *
                 * See [MQTT5 Will](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901040)
                 *
                 * @return The message to be published when the connection's session is destroyed by the server or when
                 * the will delay interval has elapsed, whichever comes first.
                 */
                const Crt::Optional<std::shared_ptr<PublishPacket>> &getWill() const noexcept;

                /**
                 * List of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901054)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                /**
                 * Intended for internal use only.  Initializes the C aws_mqtt5_packet_connack_view
                 * from PacketConnect
                 *
                 * @param raw_options - output parameter containing low level client options to be passed to the C
                 * @param allocator - memory Allocator
                 *
                 */
                bool initializeRawOptions(aws_mqtt5_packet_connect_view &raw_options, Allocator *allocator) noexcept;

                virtual ~ConnectPacket();
                ConnectPacket(const ConnectPacket &) = delete;
                ConnectPacket(ConnectPacket &&) noexcept = delete;
                ConnectPacket &operator=(const ConnectPacket &) = delete;
                ConnectPacket &operator=(ConnectPacket &&) noexcept = delete;

              private:
                Allocator *m_allocator;

                /**
                 * The maximum time interval, in seconds, that is permitted to elapse between the point at which the
                 * client finishes transmitting one MQTT packet and the point it starts sending the next.  The client
                 * will use PINGREQ packets to maintain this property.
                 *
                 * If the responding CONNACK contains a keep alive property value, then that is the negotiated keep
                 * alive value. Otherwise, the keep alive sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Keep Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901045)
                 */
                uint16_t m_keepAliveIntervalSec;

                /**
                 * A unique string identifying the client to the server.  Used to restore session state between
                 * connections.
                 *
                 * If left empty, the broker will auto-assign a unique client id.  When reconnecting, the mqtt5 client
                 * will always use the auto-assigned client id.
                 *
                 * See [MQTT5 Client
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901059)
                 */
                Crt::String m_clientId;

                /**
                 * A string value that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 User Name](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901071)
                 */
                Crt::Optional<Crt::String> m_username;

                /**
                 * Opaque binary data that the server may use for client authentication and authorization.
                 *
                 * See [MQTT5 Password](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901072)
                 */
                Crt::Optional<ByteCursor> m_password;

                /**
                 * A time interval, in seconds, that the client requests the server to persist this connection's MQTT
                 * session state for.  Has no meaning if the client has not been configured to rejoin sessions.  Must be
                 * non-zero in order to successfully rejoin a session.
                 *
                 * If the responding CONNACK contains a session expiry property value, then that is the negotiated
                 * session expiry value.  Otherwise, the session expiry sent by the client is the negotiated value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901048)
                 */
                Crt::Optional<uint32_t> m_sessionExpiryIntervalSec;

                /**
                 * If set to true, requests that the server send response information in the subsequent CONNACK.  This
                 * response information may be used to set up request-response implementations over MQTT, but doing so
                 * is outside the scope of the MQTT5 spec and client.
                 *
                 * See [MQTT5 Request Response
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901052)
                 */
                Crt::Optional<bool> m_requestResponseInformation;

                /**
                 * If set to true, requests that the server send additional diagnostic information (via response string
                 * or user properties) in DISCONNECT or CONNACK packets from the server.
                 *
                 * See [MQTT5 Request Problem
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901053)
                 */
                Crt::Optional<bool> m_requestProblemInformation;

                /**
                 * Notifies the server of the maximum number of in-flight Qos 1 and 2 messages the client is willing to
                 * handle.  If omitted, then no limit is requested.
                 *
                 * See [MQTT5 Receive
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901049)
                 */
                Crt::Optional<uint16_t> m_receiveMaximum;

                /**
                 * Notifies the server of the maximum packet size the client is willing to handle.  If
                 * omitted, then no limit beyond the natural limits of MQTT packet size is requested.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901050)
                 */
                Crt::Optional<uint32_t> m_maximumPacketSizeBytes;

                /**
                 * A time interval, in seconds, that the server should wait (for a session reconnection) before sending
                 * the will message associated with the connection's session.  If omitted, the server will send the will
                 * when the associated session is destroyed.  If the session is destroyed before a will delay interval
                 * has elapsed, then the will must be sent at the time of session destruction.
                 *
                 * See [MQTT5 Will Delay
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901062)
                 */
                Crt::Optional<uint32_t> m_willDelayIntervalSeconds;

                /**
                 * The definition of a message to be published when the connection's session is destroyed by the server
                 * or when the will delay interval has elapsed, whichever comes first.  If undefined, then nothing will
                 * be sent.
                 *
                 * See [MQTT5 Will](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901040)
                 */
                Crt::Optional<std::shared_ptr<PublishPacket>> m_will;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901054)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                ///////////////////////////////////////////////////////////////////////////
                // Underlying data storage for internal use
                ///////////////////////////////////////////////////////////////////////////
                struct aws_byte_cursor m_usernameCursor;
                struct aws_byte_buf m_passowrdStorage;
                struct aws_mqtt5_packet_publish_view m_willStorage;
                struct aws_mqtt5_user_property *m_userPropertiesStorage;
                uint8_t m_requestResponseInformationStorage;
                uint8_t m_requestProblemInformationStorage;
            };

            /**
             * Data model of an [MQTT5
             * CONNACK](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901074) packet.
             */
            class AWS_CRT_CPP_API ConnAckPacket : public IPacket
            {
              public:
                ConnAckPacket(
                    const aws_mqtt5_packet_connack_view &packet,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_CONNACK; };

                /**
                 * True if the client rejoined an existing session on the server, false otherwise.
                 *
                 * See [MQTT5 Session
                 * Present](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901078)
                 *
                 * @return True if the client rejoined an existing session on the server, false otherwise.
                 */
                bool getSessionPresent() const noexcept;

                /**
                 * Indicates either success or the reason for failure for the connection attempt.
                 *
                 * See [MQTT5 Connect Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901079)
                 *
                 * @return Code indicating either success or the reason for failure for the connection attempt.
                 */
                ConnectReasonCode getReasonCode() const noexcept;

                /**
                 * A time interval, in seconds, that the server will persist this connection's MQTT session state
                 * for.  If present, this value overrides any session expiry specified in the preceding CONNECT packet.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901082)
                 *
                 * @return A time interval, in seconds, that the server will persist this connection's MQTT session
                 * state for.
                 */
                const Crt::Optional<uint32_t> &getSessionExpiryIntervalSec() const noexcept;

                /**
                 * @deprecated The function is deprecated, please use `ConnAckPacket::getSessionExpiryIntervalSec()`.
                 *
                 * A time interval, in seconds, that the server will persist this connection's MQTT session state
                 * for.  If present, this value overrides any session expiry specified in the preceding CONNECT packet.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901082)
                 *
                 * @return A time interval, in seconds, that the server will persist this connection's MQTT session
                 * state for.
                 */
                const Crt::Optional<uint32_t> &getSessionExpiryInterval() const noexcept;

                /**
                 * The maximum amount of in-flight QoS 1 or 2 messages that the server is willing to handle at once. If
                 * omitted or null, the limit is based on the valid MQTT packet id space (65535).
                 *
                 * See [MQTT5 Receive
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901083)
                 *
                 * @return The maximum amount of in-flight QoS 1 or 2 messages that the server is willing to handle at
                 * once.
                 */
                const Crt::Optional<uint16_t> &getReceiveMaximum() const noexcept;

                /**
                 * The maximum message delivery quality of service that the server will allow on this connection.
                 *
                 * See [MQTT5 Maximum QoS](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901084)
                 *
                 * @return The maximum message delivery quality of service that the server will allow on this
                 * connection.
                 */
                const Crt::Optional<QOS> &getMaximumQOS() const noexcept;

                /**
                 * Indicates whether the server supports retained messages.  If null, retained messages are
                 * supported.
                 *
                 * See [MQTT5 Retain
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901085)
                 *
                 * @return Whether the server supports retained messages
                 */
                const Crt::Optional<bool> &getRetainAvailable() const noexcept;

                /**
                 * Specifies the maximum packet size, in bytes, that the server is willing to accept.  If null, there
                 * is no limit beyond what is imposed by the MQTT spec itself.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901086)
                 *
                 * @return The maximum packet size, in bytes, that the server is willing to accept.
                 */
                const Crt::Optional<uint32_t> &getMaximumPacketSize() const noexcept;

                /**
                 * Specifies a client identifier assigned to this connection by the server.  Only valid when the client
                 * id of the preceding CONNECT packet was left empty.
                 *
                 * See [MQTT5 Assigned Client
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901087)
                 *
                 * @return Client identifier assigned to this connection by the server
                 */
                const Crt::Optional<String> &getAssignedClientIdentifier() const noexcept;

                /**
                 * Specifies the maximum topic alias value that the server will accept from the client.
                 *
                 * See [MQTT5 Topic Alias
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901088)
                 *
                 * @return maximum topic alias
                 */
                const Crt::Optional<uint16_t> getTopicAliasMaximum() const noexcept;

                /**
                 * Additional diagnostic information about the result of the connection attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901089)
                 *
                 * @return Additional diagnostic information about the result of the connection attempt.
                 */
                const Crt::Optional<String> &getReasonString() const noexcept;

                /**
                 * List of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901090)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Vector<UserProperty> &getUserProperty() const noexcept;

                /**
                 * Indicates whether the server supports wildcard subscriptions.  If null, wildcard subscriptions
                 * are supported.
                 *
                 * See [MQTT5 Wildcard Subscriptions
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901091)
                 *
                 * @return Whether the server supports wildcard subscriptions.
                 */
                const Crt::Optional<bool> &getWildcardSubscriptionsAvailable() const noexcept;

                /**
                 * Indicates whether the server supports subscription identifiers.  If null, subscription identifiers
                 * are supported.
                 *
                 * See [MQTT5 Subscription Identifiers
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901092)
                 *
                 * @return whether the server supports subscription identifiers.
                 */
                const Crt::Optional<bool> &getSubscriptionIdentifiersAvailable() const noexcept;

                /**
                 * Indicates whether the server supports shared subscription topic filters.  If null, shared
                 * subscriptions are supported.
                 *
                 * See [MQTT5 Shared Subscriptions
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901093)
                 *
                 * @return whether the server supports shared subscription topic filters.
                 */
                const Crt::Optional<bool> &getSharedSubscriptionsAvailable() const noexcept;

                /**
                 * Server-requested override of the keep alive interval, in seconds.  If null, the keep alive value sent
                 * by the client should be used.
                 *
                 * See [MQTT5 Server Keep
                 * Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901094)
                 *
                 * @return Server-requested override of the keep alive interval, in seconds
                 */
                const Crt::Optional<uint16_t> &getServerKeepAliveSec() const noexcept;

                /**
                 * @deprecated The function is deprecated, please use `ConnAckPacket::getServerKeepAliveSec()`.
                 * Server-requested override of the keep alive interval, in seconds.  If null, the keep alive value sent
                 * by the client should be used.
                 *
                 * See [MQTT5 Server Keep
                 * Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901094)
                 *
                 * @return Server-requested override of the keep alive interval, in seconds
                 */
                const Crt::Optional<uint16_t> &getServerKeepAlive() const noexcept;

                /**
                 * A value that can be used in the creation of a response topic associated with this connection.
                 * MQTT5-based request/response is outside the purview of the MQTT5 spec and this client.
                 *
                 * See [MQTT5 Response
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901095)
                 *
                 * @return A value that can be used in the creation of a response topic associated with this connection.
                 */
                const Crt::Optional<String> &getResponseInformation() const noexcept;

                /**
                 * Property indicating an alternate server that the client may temporarily or permanently attempt
                 * to connect to instead of the configured endpoint.  Will only be set if the reason code indicates
                 * another server may be used (ServerMoved, UseAnotherServer).
                 *
                 * See [MQTT5 Server
                 * Reference](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901096)
                 *
                 * @return Property indicating an alternate server that the client may temporarily or permanently
                 * attempt to connect to instead of the configured endpoint.
                 */
                const Crt::Optional<String> &getServerReference() const noexcept;

                virtual ~ConnAckPacket() {};
                ConnAckPacket(const ConnAckPacket &) = delete;
                ConnAckPacket(ConnAckPacket &&) noexcept = delete;
                ConnAckPacket &operator=(const ConnAckPacket &) = delete;
                ConnAckPacket &operator=(ConnAckPacket &&) noexcept = delete;

              private:
                /**
                 * True if the client rejoined an existing session on the server, false otherwise.
                 *
                 * See [MQTT5 Session
                 * Present](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901078)
                 */
                bool m_sessionPresent;

                /**
                 * Indicates either success or the reason for failure for the connection attempt.
                 *
                 * See [MQTT5 Connect Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901079)
                 */
                ConnectReasonCode m_reasonCode;

                /**
                 * A time interval, in seconds, that the server will persist this connection's MQTT session state
                 * for.  If present, this value overrides any session expiry specified in the preceding CONNECT packet.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901082)
                 */
                Crt::Optional<uint32_t> m_sessionExpiryIntervalSec;

                /**
                 * The maximum amount of in-flight QoS 1 or 2 messages that the server is willing to handle at once.  If
                 * omitted, the limit is based on the valid MQTT packet id space (65535).
                 *
                 * See [MQTT5 Receive
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901083)
                 */
                Crt::Optional<uint16_t> m_receiveMaximum;

                /**
                 * The maximum message delivery quality of service that the server will allow on this connection.
                 *
                 * See [MQTT5 Maximum QoS](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901084)
                 */
                Crt::Optional<QOS> m_maximumQOS;

                /**
                 * Indicates whether the server supports retained messages.  If undefined, retained messages are
                 * supported.
                 *
                 * See [MQTT5 Retain
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901085)
                 */
                Crt::Optional<bool> m_retainAvailable;

                /**
                 * Specifies the maximum packet size, in bytes, that the server is willing to accept.  If undefined,
                 * there is no limit beyond what is imposed by the MQTT spec itself.
                 *
                 * See [MQTT5 Maximum Packet
                 * Size](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901086)
                 */
                Crt::Optional<uint32_t> m_maximumPacketSize;

                /**
                 * Specifies a client identifier assigned to this connection by the server.  Only valid when the client
                 * id of the preceding CONNECT packet was left empty.
                 *
                 * See [MQTT5 Assigned Client
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901087)
                 */
                Crt::Optional<String> m_assignedClientIdentifier;

                /**
                 * Specifies the maximum topic alias value that the server will accept from the client.
                 *
                 * See [MQTT5 Topic Alias
                 * Maximum](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901088)
                 */
                Crt::Optional<uint16_t> m_topicAliasMaximum;

                /**
                 * Additional diagnostic information about the result of the connection attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901089)
                 */
                Crt::Optional<String> m_reasonString;

                /**
                 * Indicates whether the server supports wildcard subscriptions.  If undefined, wildcard subscriptions
                 * are supported.
                 *
                 * See [MQTT5 Wildcard Subscriptions
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901091)
                 */
                Crt::Optional<bool> m_wildcardSubscriptionsAvailable;

                /**
                 * Indicates whether the server supports subscription identifiers.  If undefined, subscription
                 * identifiers are supported.
                 *
                 * See [MQTT5 Subscription Identifiers
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901092)
                 */
                Crt::Optional<bool> m_subscriptionIdentifiersAvailable;

                /**
                 * Indicates whether the server supports shared subscription topic filters.  If undefined, shared
                 * subscriptions are supported.
                 *
                 * See [MQTT5 Shared Subscriptions
                 * Available](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901093)
                 */
                Crt::Optional<bool> m_sharedSubscriptionsAvailable;

                /**
                 * Server-requested override of the keep alive interval, in seconds.  If undefined, the keep alive value
                 * sent by the client should be used.
                 *
                 * See [MQTT5 Server Keep
                 * Alive](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901094)
                 */
                Crt::Optional<uint16_t> m_serverKeepAliveSec;

                /**
                 * A value that can be used in the creation of a response topic associated with this connection.
                 * MQTT5-based request/response is outside the purview of the MQTT5 spec and this client.
                 *
                 * See [MQTT5 Response
                 * Information](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901095)
                 */
                Crt::Optional<String> m_responseInformation;

                /**
                 * Property indicating an alternate server that the client may temporarily or permanently attempt
                 * to connect to instead of the configured endpoint.  Will only be set if the reason code indicates
                 * another server may be used (ServerMoved, UseAnotherServer).
                 *
                 * See [MQTT5 Server
                 * Reference](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901096)
                 */
                Crt::Optional<String> m_serverReference;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901090)
                 */
                Vector<UserProperty> m_userProperties;
            };

            /**
             * Data model of an [MQTT5
             * DISCONNECT](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901205) packet.
             */
            class AWS_CRT_CPP_API DisconnectPacket : public IPacket
            {
              public:
                DisconnectPacket(Allocator *allocator = ApiAllocator()) noexcept;
                DisconnectPacket(
                    const aws_mqtt5_packet_disconnect_view &raw_options,
                    Allocator *allocator = ApiAllocator()) noexcept;
                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_DISCONNECT; };

                bool initializeRawOptions(aws_mqtt5_packet_disconnect_view &raw_options) noexcept;

                /**
                 * Sets the value indicating the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Disconnect Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901208)
                 *
                 * @param reasonCode Value indicating the reason that the sender is closing the connection
                 * @return The DisconnectPacket Object after setting the reason code.
                 */
                DisconnectPacket &WithReasonCode(const DisconnectReasonCode reasonCode) noexcept;

                /**
                 * Sets the change to the session expiry interval negotiated at connection time as part of the
                 * disconnect.  Only valid for DISCONNECT packets sent from client to server.  It is not valid to
                 * attempt to change session expiry from zero to a non-zero value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901211)
                 *
                 * @param sessionExpiryIntervalSeconds
                 * @return The DisconnectPacket Object after setting the session expiry interval.
                 */
                DisconnectPacket &WithSessionExpiryIntervalSec(const uint32_t sessionExpiryIntervalSeconds) noexcept;

                /**
                 * Sets the additional diagnostic information about the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901212)
                 *
                 * @param reasonString Additional diagnostic information about the reason that the sender is closing the
                 * connection
                 * @return The DisconnectPacket Object after setting the reason string.
                 */
                DisconnectPacket &WithReasonString(Crt::String reasonString) noexcept;

                /**
                 * Sets the property indicating an alternate server that the client may temporarily or permanently
                 * attempt to connect to instead of the configured endpoint.  Will only be set if the reason code
                 * indicates another server may be used (ServerMoved, UseAnotherServer).
                 *
                 * See [MQTT5 Server
                 * Reference](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901214)
                 *
                 * @param serverReference Property indicating an alternate server that the client may temporarily or
                 * permanently attempt to connect to instead of the configured endpoint.
                 * @return The DisconnectPacket Object after setting the server reference.
                 */
                DisconnectPacket &WithServerReference(Crt::String serverReference) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901213)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The DisconnectPacket Object after setting the user properties.
                 */
                DisconnectPacket &WithUserProperties(const Vector<UserProperty> &userProperties) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901213)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The DisconnectPacket Object after setting the user properties.
                 */
                DisconnectPacket &WithUserProperties(Vector<UserProperty> &&userProperties) noexcept;

                /**
                 * Put a MQTT5 user property to the back of the packet user property vector/list
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param property set of userProperty of MQTT5 user properties included with the packet.
                 * @return The ConnectPacket Object after setting the user property
                 */
                DisconnectPacket &WithUserProperty(UserProperty &&property) noexcept;

                /**
                 * Value indicating the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Disconnect Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901208)
                 *
                 * @return Value indicating the reason that the sender is closing the connection
                 */
                DisconnectReasonCode getReasonCode() const noexcept;

                /**
                 * A change to the session expiry interval negotiated at connection time as part of the disconnect. Only
                 * valid for DISCONNECT packets sent from client to server.  It is not valid to attempt to change
                 * session expiry from zero to a non-zero value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901211)
                 *
                 * @return A change to the session expiry interval negotiated at connection time as part of the
                 * disconnect.
                 */
                const Crt::Optional<uint32_t> &getSessionExpiryIntervalSec() const noexcept;

                /**
                 * Additional diagnostic information about the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901212)
                 *
                 * @return Additional diagnostic information about the reason that the sender is closing the connection
                 */
                const Crt::Optional<Crt::String> &getReasonString() const noexcept;

                /**
                 * Property indicating an alternate server that the client may temporarily or permanently attempt
                 * to connect to instead of the configured endpoint.  Will only be set if the reason code indicates
                 * another server may be used (ServerMoved, UseAnotherServer).
                 *
                 * See [MQTT5 Server
                 * Reference](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901214)
                 *
                 * @return Property indicating an alternate server that the client may temporarily or permanently
                 * attempt to connect to instead of the configured endpoint.
                 */
                const Crt::Optional<Crt::String> &getServerReference() const noexcept;

                /**
                 * List of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901213)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                virtual ~DisconnectPacket();
                DisconnectPacket(const DisconnectPacket &) = delete;
                DisconnectPacket(DisconnectPacket &&) noexcept = delete;
                DisconnectPacket &operator=(const DisconnectPacket &) = delete;
                DisconnectPacket &operator=(DisconnectPacket &&) noexcept = delete;

              private:
                Crt::Allocator *m_allocator;

                /**
                 * Value indicating the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Disconnect Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901208)
                 */
                DisconnectReasonCode m_reasonCode;

                /**
                 * Requests a change to the session expiry interval negotiated at connection time as part of the
                 * disconnect.  Only valid for  DISCONNECT packets sent from client to server.  It is not valid to
                 * attempt to change session expiry from zero to a non-zero value.
                 *
                 * See [MQTT5 Session Expiry
                 * Interval](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901211)
                 */
                Crt::Optional<uint32_t> m_sessionExpiryIntervalSec;

                /**
                 * Additional diagnostic information about the reason that the sender is closing the connection
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901212)
                 */
                Crt::Optional<Crt::String> m_reasonString;

                /**
                 * Property indicating an alternate server that the client may temporarily or permanently attempt
                 * to connect to instead of the configured endpoint.  Will only be set if the reason code indicates
                 * another server may be used (ServerMoved, UseAnotherServer).
                 *
                 * See [MQTT5 Server
                 * Reference](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901214)
                 */
                Crt::Optional<Crt::String> m_serverReference;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901213)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                ///////////////////////////////////////////////////////////////////////////
                // Underlying data storage for internal use
                ///////////////////////////////////////////////////////////////////////////
                struct aws_byte_cursor m_reasonStringCursor;
                struct aws_byte_cursor m_serverReferenceCursor;
                struct aws_mqtt5_user_property *m_userPropertiesStorage;
            };

            /**
             * Data model of an [MQTT5
             * PUBACK](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901121) packet
             */
            class AWS_CRT_CPP_API PubAckPacket : public IPacket
            {
              public:
                PubAckPacket(
                    const aws_mqtt5_packet_puback_view &packet,
                    Allocator *allocator = ApiAllocator()) noexcept;

                PacketType getType() override { return PacketType::AWS_MQTT5_PT_PUBACK; };

                /**
                 * Success indicator or failure reason for the associated PUBLISH packet.
                 *
                 * See [MQTT5 PUBACK Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124)
                 *
                 * @return Success indicator or failure reason for the associated PUBLISH packet.
                 */
                PubAckReasonCode getReasonCode() const noexcept;

                /**
                 * Additional diagnostic information about the result of the PUBLISH attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901127)
                 *
                 * @return Additional diagnostic information about the result of the PUBLISH attempt.
                 */
                const Crt::Optional<Crt::String> &getReasonString() const noexcept;

                /**
                 * List of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901128)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                virtual ~PubAckPacket() {};
                PubAckPacket(const PubAckPacket &toCopy) noexcept = delete;
                PubAckPacket(PubAckPacket &&toMove) noexcept = delete;
                PubAckPacket &operator=(const PubAckPacket &toCopy) noexcept = delete;
                PubAckPacket &operator=(PubAckPacket &&toMove) noexcept = delete;

              private:
                /**
                 * Success indicator or failure reason for the associated PUBLISH packet.
                 *
                 * See [MQTT5 PUBACK Reason
                 * Code](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901124)
                 */
                PubAckReasonCode m_reasonCode;

                /**
                 * Additional diagnostic information about the result of the PUBLISH attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901127)
                 */
                Crt::Optional<Crt::String> m_reasonString;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901128)
                 */
                Crt::Vector<UserProperty> m_userProperties;
            };

            /**
             * PublishResult returned with onPublishCompletionCallback after Publish get called
             *
             * Publish with QoS0: Ack will be nullptr
             *              QoS1: Ack will contains a PubAckPacket
             */
            class AWS_CRT_CPP_API PublishResult
            {
              public:
                PublishResult();                                     // QoS 0 success
                PublishResult(std::shared_ptr<PubAckPacket> puback); // Qos 1 success
                PublishResult(int errorCode);                        // any failure

                /**
                 * Get if the publish succeed or not
                 *
                 * @return true if error code == 0 and publish succeed
                 */
                bool wasSuccessful() const { return m_errorCode == 0; };

                /**
                 * Get the error code value
                 *
                 * @return the error code
                 */
                int getErrorCode() const { return m_errorCode; };

                /**
                 * Get Publish ack packet
                 *
                 * @return std::shared_ptr<IPacket> contains a PubAckPacket if client Publish with QoS1, otherwise
                 * nullptr.
                 */
                std::shared_ptr<IPacket> getAck() const { return m_ack; };

                ~PublishResult() noexcept;
                PublishResult(const PublishResult &toCopy) noexcept = delete;
                PublishResult(PublishResult &&toMove) noexcept = delete;
                PublishResult &operator=(const PublishResult &toCopy) noexcept = delete;
                PublishResult &operator=(PublishResult &&toMove) noexcept = delete;

              private:
                std::shared_ptr<IPacket> m_ack;
                int m_errorCode;
            };

            /**
             * Configures a single subscription within a Subscribe operation
             *
             * See [MQTT5 Subscription
             * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
             */
            class AWS_CRT_CPP_API Subscription
            {

              public:
                Subscription(Allocator *allocator = ApiAllocator());
                Subscription(Crt::String topicFilter, Mqtt5::QOS qos, Allocator *allocator = ApiAllocator());

                /**
                 * Sets topic filter to subscribe to
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param topicFilter string
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithTopicFilter(Crt::String topicFilter) noexcept;

                /**
                 * Sets Maximum QoS on which the subscriber will accept publish messages.  Negotiated QoS may be
                 * different.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param QOS
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithQOS(Mqtt5::QOS QOS) noexcept;

                /**
                 * Sets should the server not send publishes to a client when that client was the one who sent the
                 * publish? The value will be default to false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param noLocal bool
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithNoLocal(bool noLocal) noexcept;

                /**
                 * Sets should the server not send publishes to a client when that client was the one who sent the
                 * publish? The value will be default to false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param retain bool
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithRetainAsPublished(bool retain) noexcept;

                /**
                 * @deprecated The function is deprecated, please use `Subscription::WithRetainAsPublished(bool)`.
                 *
                 * Sets should the server not send publishes to a client when that client was the one who sent the
                 * publish? The value will be default to false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param retain bool
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithRetain(bool retain) noexcept;

                /**
                 * Sets should messages sent due to this subscription keep the retain flag preserved on the message?
                 * The value will be default to false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 *
                 * @param retainHandlingType
                 * @return The Subscription Object after setting the reason string.
                 */
                Subscription &WithRetainHandlingType(RetainHandlingType retainHandlingType) noexcept;

                bool initializeRawOptions(aws_mqtt5_subscription_view &raw_options) const noexcept;

                virtual ~Subscription() {};
                Subscription(const Subscription &) noexcept;
                Subscription(Subscription &&) noexcept;
                Subscription &operator=(const Subscription &) noexcept;
                Subscription &operator=(Subscription &&) noexcept;

              private:
                Allocator *m_allocator;

                /**
                 * Topic filter to subscribe to
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 */
                Crt::String m_topicFilter;

                /**
                 * Maximum QoS on which the subscriber will accept publish messages.  Negotiated QoS may be different.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 */
                Mqtt5::QOS m_qos;

                /**
                 * Should the server not send publishes to a client when that client was the one who sent the publish?
                 * If undefined, this is assumed to be false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 */
                bool m_noLocal;

                /**
                 * Should messages sent due to this subscription keep the retain flag preserved on the message?  If
                 * undefined, this is assumed to be false.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 */
                bool m_retainAsPublished;

                /**
                 * Should retained messages on matching topics be sent in reaction to this subscription?  If undefined,
                 * this is assumed to be RetainHandlingType.SendOnSubscribe.
                 *
                 * See [MQTT5 Subscription
                 * Options](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901169)
                 */
                RetainHandlingType m_retainHnadlingType;
            };

            /**
             * Data model of an [MQTT5
             * SUBSCRIBE](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901161) packet.
             */
            class AWS_CRT_CPP_API SubscribePacket : public IPacket
            {
              public:
                SubscribePacket(Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_SUBSCRIBE; };

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return the SubscribePacket Object after setting the reason string.
                 */
                SubscribePacket &WithUserProperties(const Vector<UserProperty> &userProperties) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return the SubscribePacket Object after setting the reason string.
                 */
                SubscribePacket &WithUserProperties(Vector<UserProperty> &&userProperties) noexcept;

                /**
                 * Put a MQTT5 user property to the back of the packet user property vector/list
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param property userProperty of MQTT5 user properties included with the packet.
                 * @return The SubscribePacket Object after setting the user property
                 */
                SubscribePacket &WithUserProperty(UserProperty &&property) noexcept;

                /**
                 * Sets the value to associate with all subscriptions in this request.  Publish packets that
                 * match a subscription in this request should include this identifier in the resulting message.
                 *
                 * See [MQTT5 Subscription
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901166)
                 *
                 * @param subscriptionIdentifier A positive long to associate with all subscriptions in this request.
                 * @return The SubscribePacket Object after setting the subscription identifier.
                 */
                SubscribePacket &WithSubscriptionIdentifier(uint32_t subscriptionIdentifier) noexcept;

                /**
                 * Sets a list of subscriptions within the SUBSCRIBE packet.
                 *
                 * @param subscriptions vector of subscriptions to add within the SUBSCRIBE packet.
                 *
                 * @return The SubscribePacket Object after setting the subscription.
                 */
                SubscribePacket &WithSubscriptions(const Vector<Subscription> &subscriptions) noexcept;

                /**
                 * Sets a list of subscriptions within the SUBSCRIBE packet.
                 *
                 * @param subscriptions vector of subscriptions to add within the SUBSCRIBE packet.
                 *
                 * @return The SubscribePacket Object after setting the subscription.
                 */
                SubscribePacket &WithSubscriptions(Crt::Vector<Subscription> &&subscriptions) noexcept;

                /**
                 * Sets a single subscription within the SUBSCRIBE packet.
                 *
                 * @param subscription The subscription to add within the SUBSCRIBE packet.
                 *
                 * @return The SubscribePacket Object after setting the subscription.
                 */
                SubscribePacket &WithSubscription(Subscription &&subscription) noexcept;

                bool initializeRawOptions(aws_mqtt5_packet_subscribe_view &raw_options) noexcept;

                virtual ~SubscribePacket();
                SubscribePacket(const SubscribePacket &) noexcept = delete;
                SubscribePacket(SubscribePacket &&) noexcept = delete;
                SubscribePacket &operator=(const SubscribePacket &) noexcept = delete;
                SubscribePacket &operator=(SubscribePacket &&) noexcept = delete;

              private:
                Allocator *m_allocator;

                /**
                 * List of topic filter subscriptions that the client wishes to listen to
                 *
                 * See [MQTT5 Subscribe
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901168)
                 */
                Crt::Vector<Subscription> m_subscriptions;

                /**
                 * A positive integer to associate with all subscriptions in this request.  Publish packets that match
                 * a subscription in this request should include this identifier in the resulting message.
                 *
                 * See [MQTT5 Subscription
                 * Identifier](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901166)
                 */
                Crt::Optional<uint32_t> m_subscriptionIdentifier;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901167)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                ///////////////////////////////////////////////////////////////////////////
                // Underlying data storage for internal use
                ///////////////////////////////////////////////////////////////////////////
                struct aws_mqtt5_subscription_view *m_subscriptionViewStorage;
                struct aws_mqtt5_user_property *m_userPropertiesStorage;
            };

            /**
             * Data model of an [MQTT5
             * SUBACK](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901171) packet.
             */
            class AWS_CRT_CPP_API SubAckPacket : public IPacket
            {
              public:
                SubAckPacket(
                    const aws_mqtt5_packet_suback_view &packet,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_SUBACK; };

                /**
                 * Returns additional diagnostic information about the result of the SUBSCRIBE attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901176)
                 *
                 * @return Additional diagnostic information about the result of the SUBSCRIBE attempt.
                 */
                const Crt::Optional<Crt::String> &getReasonString() const noexcept;

                /**
                 * Returns list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901177)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                /**
                 * Returns list of reason codes indicating the result of each individual subscription entry in the
                 * associated SUBSCRIBE packet.
                 *
                 * See [MQTT5 Suback
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901178)
                 *
                 * @return list of reason codes indicating the result of each individual subscription entry in the
                 * associated SUBSCRIBE packet.
                 */
                const Crt::Vector<SubAckReasonCode> &getReasonCodes() const noexcept;

                virtual ~SubAckPacket() { m_userProperties.clear(); };
                SubAckPacket(const SubAckPacket &) noexcept = delete;
                SubAckPacket(SubAckPacket &&) noexcept = delete;
                SubAckPacket &operator=(const SubAckPacket &) noexcept = delete;
                SubAckPacket &operator=(SubAckPacket &&) noexcept = delete;

              private:
                /**
                 * A list of reason codes indicating the result of each individual subscription entry in the
                 * associated SUBSCRIBE packet.
                 *
                 * See [MQTT5 Suback
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901178)
                 */
                Crt::Vector<SubAckReasonCode> m_reasonCodes;

                /**
                 * Additional diagnostic information about the result of the SUBSCRIBE attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901176)
                 */
                Crt::Optional<Crt::String> m_reasonString;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901177)
                 */
                Crt::Vector<UserProperty> m_userProperties;
            };

            /**
             * Data model of an [MQTT5
             * UNSUBSCRIBE](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901179) packet.
             */
            class AWS_CRT_CPP_API UnsubscribePacket : public IPacket
            {
              public:
                UnsubscribePacket(Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_UNSUBSCRIBE; };

                /**
                 * Push back a topic filter that the client wishes to unsubscribe from.
                 *
                 * @param topicFilter that the client wishes to unsubscribe from
                 *
                 * @return The UnsubscribePacket Object after setting the subscription.
                 */
                UnsubscribePacket &WithTopicFilter(Crt::String topicFilter) noexcept;

                /**
                 * Sets list of topic filter that the client wishes to unsubscribe from.
                 *
                 * @param topicFilters vector of subscription topic filters that the client wishes to unsubscribe from
                 *
                 * @return The UnsubscribePacket Object after setting the subscription.
                 */
                UnsubscribePacket &WithTopicFilters(Crt::Vector<String> topicFilters) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901184)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The UnsubscribePacketBuilder after setting the user properties.
                 */
                UnsubscribePacket &WithUserProperties(const Vector<UserProperty> &userProperties) noexcept;

                /**
                 * Sets the list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901184)
                 *
                 * @param userProperties List of MQTT5 user properties included with the packet.
                 * @return The UnsubscribePacketBuilder after setting the user properties.
                 */
                UnsubscribePacket &WithUserProperties(Vector<UserProperty> &&userProperties) noexcept;

                /**
                 * Put a MQTT5 user property to the back of the packet user property vector/list
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901116)
                 *
                 * @param property set of userProperty of MQTT5 user properties included with the packet.
                 * @return The PublishPacket Object after setting the user property
                 */
                UnsubscribePacket &WithUserProperty(UserProperty &&property) noexcept;

                bool initializeRawOptions(aws_mqtt5_packet_unsubscribe_view &raw_options) noexcept;

                virtual ~UnsubscribePacket();
                UnsubscribePacket(const UnsubscribePacket &) noexcept = delete;
                UnsubscribePacket(UnsubscribePacket &&) noexcept = delete;
                UnsubscribePacket &operator=(const UnsubscribePacket &) noexcept = delete;
                UnsubscribePacket &operator=(UnsubscribePacket &&) noexcept = delete;

              private:
                Allocator *m_allocator;

                /**
                 * List of topic filters that the client wishes to unsubscribe from.
                 *
                 * See [MQTT5 Unsubscribe
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901185)
                 */
                Crt::Vector<String> m_topicFilters;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901184)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                ///////////////////////////////////////////////////////////////////////////
                // Underlying data storage for internal use
                ///////////////////////////////////////////////////////////////////////////
                struct aws_array_list m_topicFiltersList;
                struct aws_mqtt5_user_property *m_userPropertiesStorage;
            };

            /**
             * Data model of an [MQTT5
             * UNSUBACK](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901187) packet.
             */
            class AWS_CRT_CPP_API UnSubAckPacket : public IPacket
            {
              public:
                UnSubAckPacket(
                    const aws_mqtt5_packet_unsuback_view &packet,
                    Allocator *allocator = ApiAllocator()) noexcept;

                /* The packet type */
                PacketType getType() override { return PacketType::AWS_MQTT5_PT_UNSUBACK; };

                /**
                 * Returns additional diagnostic information about the result of the UNSUBSCRIBE attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901192)
                 *
                 * @return Additional diagnostic information about the result of the UNSUBSCRIBE attempt.
                 */
                const Crt::Optional<Crt::String> &getReasonString() const noexcept;

                /**
                 * Returns list of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901193)
                 *
                 * @return List of MQTT5 user properties included with the packet.
                 */
                const Crt::Vector<UserProperty> &getUserProperties() const noexcept;

                /**
                 * Returns a list of reason codes indicating the result of unsubscribing from each individual topic
                 * filter entry in the associated UNSUBSCRIBE packet.
                 *
                 * See [MQTT5 Unsuback
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901194)
                 *
                 * @return A list of reason codes indicating the result of unsubscribing from each individual topic
                 * filter entry in the associated UNSUBSCRIBE packet.
                 */
                const Crt::Vector<UnSubAckReasonCode> &getReasonCodes() const noexcept;

                virtual ~UnSubAckPacket() { m_userProperties.clear(); };
                UnSubAckPacket(const UnSubAckPacket &) noexcept = delete;
                UnSubAckPacket(UnSubAckPacket &&) noexcept = delete;
                UnSubAckPacket &operator=(const UnSubAckPacket &) noexcept = delete;
                UnSubAckPacket &operator=(UnSubAckPacket &&) noexcept = delete;

              private:
                /**
                 * Additional diagnostic information about the result of the UNSUBSCRIBE attempt.
                 *
                 * See [MQTT5 Reason
                 * String](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901192)
                 */
                Crt::Optional<Crt::String> m_reasonString;

                /**
                 * Set of MQTT5 user properties included with the packet.
                 *
                 * See [MQTT5 User
                 * Property](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901193)
                 */
                Crt::Vector<UserProperty> m_userProperties;

                /**
                 * A list of reason codes indicating the result of unsubscribing from each individual topic filter entry
                 * in the associated UNSUBSCRIBE packet.
                 *
                 * See [MQTT5 Unsuback
                 * Payload](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc3901194)
                 */
                Crt::Vector<UnSubAckReasonCode> m_reasonCodes;
            };

        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
