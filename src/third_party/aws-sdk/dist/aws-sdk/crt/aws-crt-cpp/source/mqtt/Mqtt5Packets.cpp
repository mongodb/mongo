/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/Mqtt5Packets.h>

namespace Aws
{
    namespace Crt
    {
        namespace Mqtt5
        {
            template <typename T> void setPacketVector(Vector<T> &vector, const T *values, size_t length)
            {
                vector.clear();
                for (size_t i = 0; i < length; ++i)
                {
                    vector.push_back(values[i]);
                }
            }
            template <typename T> void setPacketOptional(Optional<T> &optional, const T *value)
            {
                if (value != nullptr)
                {
                    optional = *value;
                }
                else
                {
                    optional.reset();
                }
            }

            void setPacketStringOptional(
                Optional<aws_byte_cursor> &optional,
                Crt::String &optionalStorage,
                const aws_byte_cursor *value)
            {
                if (value != nullptr)
                {
                    optionalStorage = Crt::String((const char *)value->ptr, value->len);
                    struct aws_byte_cursor optional_cursor;
                    optional_cursor.ptr = (uint8_t *)optionalStorage.c_str();
                    optional_cursor.len = optionalStorage.size();
                    optional = optional_cursor;
                }
            }

            void setPacketStringOptional(Optional<Crt::String> &optional, const aws_byte_cursor *value)
            {
                if (value != nullptr)
                {
                    optional = Crt::String((const char *)value->ptr, value->len);
                }
                else
                {
                    optional.reset();
                }
            }

            void setPacketStringOptional(Optional<Crt::String> &optional, Crt::String &&toMove)
            {
                if (!toMove.empty())
                {
                    optional = std::move(toMove);
                }
                else
                {
                    optional.reset();
                }
            }

            void setPacketByteBufOptional(
                Optional<aws_byte_cursor> &optional,
                ByteBuf &optionalStorage,
                Allocator *allocator,
                const aws_byte_cursor *value)
            {
                aws_byte_buf_clean_up(&optionalStorage);
                AWS_ZERO_STRUCT(optionalStorage);
                if (value != nullptr)
                {
                    aws_byte_buf_init_copy_from_cursor(&optionalStorage, allocator, *value);
                    optional = aws_byte_cursor_from_buf(&optionalStorage);
                }
                else
                {
                    optional.reset();
                }
            }
            void setUserProperties(
                Vector<UserProperty> &userProperties,
                const struct aws_mqtt5_user_property *properties,
                size_t propertyCount)
            {
                for (size_t i = 0; i < propertyCount; ++i)
                {
                    userProperties.push_back(UserProperty(
                        Aws::Crt::String((const char *)properties[i].name.ptr, properties[i].name.len),
                        Aws::Crt::String((const char *)properties[i].value.ptr, properties[i].value.len)));
                }
            }
            template <typename T> void setNullableFromOptional(const T *&nullable, const Optional<T> &optional)
            {
                if (optional.has_value())
                {
                    nullable = &optional.value();
                }
            }

            void s_AllocateUnderlyingUserProperties(
                aws_mqtt5_user_property *&dst,
                const Crt::Vector<UserProperty> &userProperties,
                Allocator *allocator)
            {
                if (dst != nullptr)
                {
                    aws_mem_release(allocator, (void *)dst);
                    dst = nullptr;
                }
                if (userProperties.size() > 0)
                {
                    dst = reinterpret_cast<struct aws_mqtt5_user_property *>(
                        aws_mem_calloc(allocator, userProperties.size(), sizeof(aws_mqtt5_user_property)));
                    AWS_ZERO_STRUCT(*dst);
                    for (size_t index = 0; index < userProperties.size(); ++index)
                    {
                        (dst + index)->name = aws_byte_cursor_from_array(
                            userProperties[index].getName().c_str(), userProperties[index].getName().length());
                        (dst + index)->value = aws_byte_cursor_from_array(
                            userProperties[index].getValue().c_str(), userProperties[index].getValue().length());
                    }
                }
            }

            void s_AllocateStringVector(
                aws_array_list &dst,
                const Crt::Vector<String> &stringVector,
                Allocator *allocator)
            {
                aws_array_list_clean_up(&dst);

                if (aws_array_list_init_dynamic(&dst, allocator, stringVector.size(), sizeof(aws_byte_cursor)) !=
                    AWS_OP_SUCCESS)
                {
                    return;
                }

                for (auto &topic : stringVector)
                {
                    ByteCursor topicCursor = ByteCursorFromString(topic);
                    aws_array_list_push_back(&dst, reinterpret_cast<const void *>(&topicCursor));
                }
            }

            void s_AllocateUnderlyingSubscription(
                aws_mqtt5_subscription_view *&dst,
                const Crt::Vector<Subscription> &subscriptions,
                Allocator *allocator)
            {
                if (dst != nullptr)
                {
                    aws_mem_release(allocator, dst);
                    dst = nullptr;
                }

                aws_array_list subscription_list;
                AWS_ZERO_STRUCT(subscription_list);

                if (aws_array_list_init_dynamic(
                        &subscription_list, allocator, subscriptions.size(), sizeof(aws_mqtt5_subscription_view)) !=
                    AWS_OP_SUCCESS)
                {
                    return;
                }

                for (auto &subscription : subscriptions)
                {

                    aws_mqtt5_subscription_view underlying_subscription;
                    if (subscription.initializeRawOptions(underlying_subscription) != true)
                    {
                        goto clean_up;
                    }

                    aws_array_list_push_back(
                        &subscription_list, reinterpret_cast<const void *>(&underlying_subscription));
                }
                dst = static_cast<aws_mqtt5_subscription_view *>(subscription_list.data);
                return;

            clean_up:
                aws_array_list_clean_up(&subscription_list);
            }

            ConnectPacket::ConnectPacket(Allocator *allocator) noexcept
                : m_allocator(allocator), m_keepAliveIntervalSec(1200), m_userPropertiesStorage(nullptr)
            {
                // m_clientId.clear();
                AWS_ZERO_STRUCT(m_usernameCursor);
                AWS_ZERO_STRUCT(m_passowrdStorage);
                AWS_ZERO_STRUCT(m_willStorage);
            }

            ConnectPacket &ConnectPacket::WithKeepAliveIntervalSec(uint16_t second) noexcept
            {
                m_keepAliveIntervalSec = second;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithClientId(Crt::String client_id) noexcept
            {
                m_clientId = std::move(client_id);
                return *this;
            }

            ConnectPacket &ConnectPacket::WithUserName(Crt::String username) noexcept
            {
                m_username = std::move(username);
                m_usernameCursor = ByteCursorFromString(m_username.value());
                return *this;
            }

            ConnectPacket &ConnectPacket::WithPassword(Crt::ByteCursor password) noexcept
            {
                setPacketByteBufOptional(m_password, m_passowrdStorage, m_allocator, &password);
                return *this;
            }

            ConnectPacket &ConnectPacket::WithSessionExpiryIntervalSec(uint32_t sessionExpiryIntervalSec) noexcept
            {
                m_sessionExpiryIntervalSec = sessionExpiryIntervalSec;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithRequestResponseInformation(bool requestResponseInformation) noexcept
            {
                m_requestResponseInformation = requestResponseInformation;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithRequestProblemInformation(bool requestProblemInformation) noexcept
            {
                m_requestProblemInformation = requestProblemInformation;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithReceiveMaximum(uint16_t receiveMaximum) noexcept
            {
                m_receiveMaximum = receiveMaximum;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithMaximumPacketSizeBytes(uint32_t maximumPacketSizeBytes) noexcept
            {
                m_maximumPacketSizeBytes = maximumPacketSizeBytes;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithWillDelayIntervalSec(uint32_t willDelayIntervalSec) noexcept
            {
                m_willDelayIntervalSeconds = willDelayIntervalSec;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithWill(std::shared_ptr<PublishPacket> will) noexcept
            {
                m_will = will;
                m_will.value()->initializeRawOptions(m_willStorage);
                return *this;
            }

            ConnectPacket &ConnectPacket::WithUserProperties(const Vector<UserProperty> &userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithUserProperties(Vector<UserProperty> &&userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            ConnectPacket &ConnectPacket::WithUserProperty(UserProperty &&property) noexcept
            {
                m_userProperties.push_back(std::move(property));
                return *this;
            }

            bool ConnectPacket::initializeRawOptions(
                aws_mqtt5_packet_connect_view &raw_options,
                Allocator * /*allocator*/) noexcept
            {
                AWS_ZERO_STRUCT(raw_options);

                raw_options.keep_alive_interval_seconds = m_keepAliveIntervalSec;
                raw_options.client_id = ByteCursorFromString(m_clientId);

                if (m_username.has_value())
                {
                    raw_options.username = &m_usernameCursor;
                }

                if (m_password.has_value())
                {
                    raw_options.password = &m_password.value();
                }

                if (m_sessionExpiryIntervalSec.has_value())
                {
                    raw_options.session_expiry_interval_seconds = &m_sessionExpiryIntervalSec.value();
                }

                if (m_requestResponseInformation.has_value())
                {
                    m_requestResponseInformationStorage = m_requestResponseInformation.value() ? 1 : 0;
                    raw_options.request_response_information = &m_requestResponseInformationStorage;
                }

                if (m_requestProblemInformation.has_value())
                {
                    m_requestProblemInformationStorage = m_requestProblemInformation.value() ? 1 : 0;
                    raw_options.request_problem_information = &m_requestProblemInformationStorage;
                }

                if (m_receiveMaximum.has_value())
                {
                    raw_options.receive_maximum = &m_receiveMaximum.value();
                }

                if (m_maximumPacketSizeBytes.has_value())
                {
                    raw_options.maximum_packet_size_bytes = &m_maximumPacketSizeBytes.value();
                }

                if (m_willDelayIntervalSeconds.has_value())
                {
                    raw_options.will_delay_interval_seconds = &m_willDelayIntervalSeconds.value();
                }

                if (m_will.has_value())
                {
                    raw_options.will = &m_willStorage;
                }

                s_AllocateUnderlyingUserProperties(m_userPropertiesStorage, m_userProperties, m_allocator);
                raw_options.user_properties = m_userPropertiesStorage;
                raw_options.user_property_count = m_userProperties.size();

                return true;
            }

            ConnectPacket::~ConnectPacket()
            {
                if (m_userPropertiesStorage != nullptr)
                {
                    aws_mem_release(m_allocator, m_userPropertiesStorage);
                    m_userProperties.clear();
                }
                aws_byte_buf_clean_up(&m_passowrdStorage);
            }

            uint16_t ConnectPacket::getKeepAliveIntervalSec() const noexcept
            {
                return m_keepAliveIntervalSec;
            }

            const Crt::String &ConnectPacket::getClientId() const noexcept
            {
                return m_clientId;
            }

            const Crt::Optional<Crt::String> &ConnectPacket::getUsername() const noexcept
            {
                return m_username;
            }

            const Crt::Optional<Crt::ByteCursor> &ConnectPacket::getPassword() const noexcept
            {
                return m_password;
            }

            const Crt::Optional<uint32_t> &ConnectPacket::getSessionExpiryIntervalSec() const noexcept
            {
                return m_sessionExpiryIntervalSec;
            }

            const Crt::Optional<bool> &ConnectPacket::getRequestResponseInformation() const noexcept
            {
                return m_requestResponseInformation;
            }

            const Crt::Optional<bool> &ConnectPacket::getRequestProblemInformation() const noexcept
            {
                return m_requestProblemInformation;
            }

            const Crt::Optional<uint16_t> &ConnectPacket::getReceiveMaximum() const noexcept
            {
                return m_receiveMaximum;
            }

            const Crt::Optional<uint32_t> &ConnectPacket::getMaximumPacketSizeBytes() const noexcept
            {
                return getMaximumPacketSizeToServer();
            }

            const Crt::Optional<uint32_t> &ConnectPacket::getMaximumPacketSizeToServer() const noexcept
            {
                return m_maximumPacketSizeBytes;
            }

            const Crt::Optional<uint32_t> &ConnectPacket::getWillDelayIntervalSec() const noexcept
            {
                return m_willDelayIntervalSeconds;
            }

            const Crt::Optional<std::shared_ptr<PublishPacket>> &ConnectPacket::getWill() const noexcept
            {
                return m_will;
            }

            const Crt::Vector<UserProperty> &ConnectPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            UserProperty::UserProperty(Crt::String name, Crt::String value) noexcept
                : m_name(std::move(name)), m_value(std::move(value))
            {
            }

            UserProperty::~UserProperty() noexcept {}

            UserProperty::UserProperty(const UserProperty &toCopy) noexcept
                : m_name(toCopy.getName()), m_value(toCopy.getValue())
            {
            }

            UserProperty::UserProperty(UserProperty &&toMove) noexcept
                : m_name(std::move(toMove.m_name)), m_value(std::move(toMove.m_value))
            {
            }

            UserProperty &UserProperty::operator=(const UserProperty &toCopy) noexcept
            {
                if (&toCopy != this)
                {
                    m_name = toCopy.getName();
                    m_value = toCopy.getValue();
                }
                return *this;
            }

            UserProperty &UserProperty::operator=(UserProperty &&toMove) noexcept
            {
                if (&toMove != this)
                {
                    m_name = std::move(toMove.m_name);
                    m_value = std::move(toMove.m_value);
                }
                return *this;
            }

            PublishPacket::PublishPacket(const aws_mqtt5_packet_publish_view &packet, Allocator *allocator) noexcept
                : m_allocator(allocator), m_qos(packet.qos), m_retain(packet.retain),
                  m_topicName((const char *)packet.topic.ptr, packet.topic.len), m_userPropertiesStorage(nullptr)
            {
                AWS_ZERO_STRUCT(m_payloadStorage);
                AWS_ZERO_STRUCT(m_contentTypeStorage);
                AWS_ZERO_STRUCT(m_correlationDataStorage);
                AWS_ZERO_STRUCT(m_payload);

                WithPayload(packet.payload);

                setPacketOptional(m_payloadFormatIndicator, packet.payload_format);
                setPacketOptional(m_messageExpiryIntervalSec, packet.message_expiry_interval_seconds);
                setPacketOptional(m_topicAlias, packet.topic_alias);
                setPacketStringOptional(m_responseTopic, m_responseTopicString, packet.response_topic);
                setPacketByteBufOptional(
                    m_correlationData, m_correlationDataStorage, allocator, packet.correlation_data);
                setPacketByteBufOptional(m_contentType, m_contentTypeStorage, allocator, packet.content_type);
                setPacketVector(
                    m_subscriptionIdentifiers, packet.subscription_identifiers, packet.subscription_identifier_count);
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
            }

            /* Default constructor */
            PublishPacket::PublishPacket(Allocator *allocator) noexcept
                : m_allocator(allocator), m_qos(QOS::AWS_MQTT5_QOS_AT_MOST_ONCE), m_retain(false), m_topicName(""),
                  m_userPropertiesStorage(nullptr)
            {
                AWS_ZERO_STRUCT(m_payloadStorage);
                AWS_ZERO_STRUCT(m_contentTypeStorage);
                AWS_ZERO_STRUCT(m_correlationDataStorage);
                AWS_ZERO_STRUCT(m_payload);
            }

            PublishPacket::PublishPacket(
                Crt::String topic,
                ByteCursor payload,
                Mqtt5::QOS qos,
                Allocator *allocator) noexcept
                : m_allocator(allocator), m_qos(qos), m_retain(false), m_topicName(std::move(topic)),
                  m_userPropertiesStorage(nullptr)
            {
                AWS_ZERO_STRUCT(m_payloadStorage);
                AWS_ZERO_STRUCT(m_contentTypeStorage);
                AWS_ZERO_STRUCT(m_correlationDataStorage);
                AWS_ZERO_STRUCT(m_payload);

                // Setup message payload, sync with PublishPacket::WithPayload
                aws_byte_buf_clean_up(&m_payloadStorage);
                aws_byte_buf_init_copy_from_cursor(&m_payloadStorage, m_allocator, payload);
                m_payload = aws_byte_cursor_from_buf(&m_payloadStorage);
            }

            PublishPacket &PublishPacket::WithPayload(ByteCursor payload) noexcept
            {
                aws_byte_buf_clean_up(&m_payloadStorage);
                aws_byte_buf_init_copy_from_cursor(&m_payloadStorage, m_allocator, payload);
                m_payload = aws_byte_cursor_from_buf(&m_payloadStorage);
                return *this;
            }

            PublishPacket &PublishPacket::WithQOS(Mqtt5::QOS qos) noexcept
            {
                m_qos = qos;
                return *this;
            }

            PublishPacket &PublishPacket::WithRetain(bool retain) noexcept
            {
                m_retain = retain;
                return *this;
            }

            PublishPacket &PublishPacket::WithTopic(Crt::String topic) noexcept
            {
                m_topicName = std::move(topic);
                return *this;
            }

            PublishPacket &PublishPacket::WithPayloadFormatIndicator(PayloadFormatIndicator format) noexcept
            {
                m_payloadFormatIndicator = format;
                return *this;
            }

            PublishPacket &PublishPacket::WithMessageExpiryIntervalSec(uint32_t second) noexcept
            {
                m_messageExpiryIntervalSec = second;
                return *this;
            }

            PublishPacket &PublishPacket::WithTopicAlias(uint16_t topicAlias) noexcept
            {
                m_topicAlias = topicAlias;
                return *this;
            }

            PublishPacket &PublishPacket::WithResponseTopic(ByteCursor responseTopic) noexcept
            {
                setPacketStringOptional(m_responseTopic, m_responseTopicString, &responseTopic);
                return *this;
            }

            PublishPacket &PublishPacket::WithCorrelationData(ByteCursor correlationData) noexcept
            {
                setPacketByteBufOptional(m_correlationData, m_correlationDataStorage, m_allocator, &correlationData);
                return *this;
            }

            PublishPacket &PublishPacket::WithUserProperties(const Vector<UserProperty> &userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            PublishPacket &PublishPacket::WithUserProperties(Vector<UserProperty> &&userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            PublishPacket &PublishPacket::WithUserProperty(UserProperty &&property) noexcept
            {
                m_userProperties.push_back(std::move(property));
                return *this;
            }

            bool PublishPacket::initializeRawOptions(aws_mqtt5_packet_publish_view &raw_options) noexcept
            {
                AWS_ZERO_STRUCT(raw_options);
                raw_options.payload = m_payload;
                raw_options.qos = m_qos;
                raw_options.retain = m_retain;
                raw_options.topic = ByteCursorFromString(m_topicName);

                if (m_payloadFormatIndicator.has_value())
                {
                    raw_options.payload_format =
                        (aws_mqtt5_payload_format_indicator *)&m_payloadFormatIndicator.value();
                }
                if (m_messageExpiryIntervalSec.has_value())
                {
                    raw_options.message_expiry_interval_seconds = &m_messageExpiryIntervalSec.value();
                }
                if (m_topicAlias.has_value())
                {
                    raw_options.topic_alias = &m_topicAlias.value();
                }
                if (m_responseTopic.has_value())
                {
                    raw_options.response_topic = &m_responseTopic.value();
                }
                if (m_correlationData.has_value())
                {
                    raw_options.correlation_data = &m_correlationData.value();
                }

                s_AllocateUnderlyingUserProperties(m_userPropertiesStorage, m_userProperties, m_allocator);
                raw_options.user_properties = m_userPropertiesStorage;
                raw_options.user_property_count = m_userProperties.size();

                return true;
            }

            const ByteCursor &PublishPacket::getPayload() const noexcept
            {
                return m_payload;
            }

            Mqtt5::QOS PublishPacket::getQOS() const noexcept
            {
                return m_qos;
            }

            bool PublishPacket::getRetain() const noexcept
            {
                return m_retain;
            }

            const Crt::String &PublishPacket::getTopic() const noexcept
            {
                return m_topicName;
            }

            const Crt::Optional<PayloadFormatIndicator> &PublishPacket::getPayloadFormatIndicator() const noexcept
            {
                return m_payloadFormatIndicator;
            }

            const Crt::Optional<uint32_t> &PublishPacket::getMessageExpiryIntervalSec() const noexcept
            {
                return m_messageExpiryIntervalSec;
            }

            const Crt::Optional<uint16_t> &PublishPacket::getTopicAlias() const noexcept
            {
                return m_topicAlias;
            }

            const Crt::Optional<ByteCursor> &PublishPacket::getResponseTopic() const noexcept
            {
                return m_responseTopic;
            }

            const Crt::Optional<ByteCursor> &PublishPacket::getCorrelationData() const noexcept
            {
                return m_correlationData;
            }

            const Crt::Vector<uint32_t> &PublishPacket::getSubscriptionIdentifiers() const noexcept
            {
                return m_subscriptionIdentifiers;
            }

            const Crt::Optional<ByteCursor> &PublishPacket::getContentType() const noexcept
            {
                return m_contentType;
            }

            const Crt::Vector<UserProperty> &PublishPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            PublishPacket::~PublishPacket()
            {
                aws_byte_buf_clean_up(&m_payloadStorage);
                aws_byte_buf_clean_up(&m_correlationDataStorage);
                aws_byte_buf_clean_up(&m_contentTypeStorage);

                if (m_userProperties.size() > 0)
                {
                    aws_mem_release(m_allocator, m_userPropertiesStorage);
                    m_userProperties.clear();
                }
            }

            DisconnectPacket::DisconnectPacket(Allocator *allocator) noexcept
                : m_allocator(allocator), m_reasonCode(AWS_MQTT5_DRC_NORMAL_DISCONNECTION),
                  m_userPropertiesStorage(nullptr)
            {
            }

            bool DisconnectPacket::initializeRawOptions(aws_mqtt5_packet_disconnect_view &raw_options) noexcept
            {
                AWS_ZERO_STRUCT(raw_options);

                raw_options.reason_code = m_reasonCode;

                if (m_sessionExpiryIntervalSec.has_value())
                {
                    raw_options.session_expiry_interval_seconds = &m_sessionExpiryIntervalSec.value();
                }

                if (m_reasonString.has_value())
                {
                    m_reasonStringCursor = ByteCursorFromString(m_reasonString.value());
                    raw_options.reason_string = &m_reasonStringCursor;
                }

                if (m_serverReference.has_value())
                {
                    m_serverReferenceCursor = ByteCursorFromString(m_serverReference.value());
                    raw_options.server_reference = &m_serverReferenceCursor;
                }

                s_AllocateUnderlyingUserProperties(m_userPropertiesStorage, m_userProperties, m_allocator);
                raw_options.user_properties = m_userPropertiesStorage;
                raw_options.user_property_count = m_userProperties.size();

                return true;
            }

            DisconnectPacket &DisconnectPacket::WithReasonCode(const DisconnectReasonCode code) noexcept
            {
                m_reasonCode = code;
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithSessionExpiryIntervalSec(const uint32_t second) noexcept
            {
                m_sessionExpiryIntervalSec = second;
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithReasonString(Crt::String reason) noexcept
            {
                m_reasonString = std::move(reason);
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithServerReference(Crt::String server_reference) noexcept
            {
                m_serverReference = std::move(server_reference);
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithUserProperties(const Vector<UserProperty> &userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithUserProperties(Vector<UserProperty> &&userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            DisconnectPacket &DisconnectPacket::WithUserProperty(UserProperty &&property) noexcept
            {
                m_userProperties.push_back(std::move(property));
                return *this;
            }

            DisconnectReasonCode DisconnectPacket::getReasonCode() const noexcept
            {
                return m_reasonCode;
            }

            const Crt::Optional<uint32_t> &DisconnectPacket::getSessionExpiryIntervalSec() const noexcept
            {
                return m_sessionExpiryIntervalSec;
            }

            const Crt::Optional<Crt::String> &DisconnectPacket::getReasonString() const noexcept
            {
                return m_reasonString;
            }

            const Crt::Optional<Crt::String> &DisconnectPacket::getServerReference() const noexcept
            {
                return m_serverReference;
            }

            const Crt::Vector<UserProperty> &DisconnectPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            DisconnectPacket::DisconnectPacket(
                const aws_mqtt5_packet_disconnect_view &packet,
                Allocator *allocator) noexcept
                : m_allocator(allocator), m_userPropertiesStorage(nullptr)
            {
                m_reasonCode = packet.reason_code;

                setPacketOptional(m_sessionExpiryIntervalSec, packet.session_expiry_interval_seconds);
                setPacketStringOptional(m_reasonString, packet.reason_string);
                setPacketStringOptional(m_serverReference, packet.server_reference);
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
            }

            DisconnectPacket::~DisconnectPacket()
            {
                if (m_userPropertiesStorage != nullptr)
                {
                    aws_mem_release(m_allocator, m_userPropertiesStorage);
                }
            }

            PubAckPacket::PubAckPacket(const aws_mqtt5_packet_puback_view &packet, Allocator * /*allocator*/) noexcept
            {
                m_reasonCode = packet.reason_code;
                setPacketStringOptional(m_reasonString, packet.reason_string);
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
            }

            PubAckReasonCode PubAckPacket::getReasonCode() const noexcept
            {
                return m_reasonCode;
            }

            const Crt::Optional<Crt::String> &PubAckPacket::getReasonString() const noexcept
            {
                return m_reasonString;
            }

            const Crt::Vector<UserProperty> &PubAckPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            ConnAckPacket::ConnAckPacket(
                const aws_mqtt5_packet_connack_view &packet,
                Allocator * /*allocator*/) noexcept
            {
                m_sessionPresent = packet.session_present;
                m_reasonCode = packet.reason_code;
                setPacketOptional(m_sessionExpiryIntervalSec, packet.session_expiry_interval);
                setPacketOptional(m_receiveMaximum, packet.receive_maximum);
                setPacketOptional(m_maximumQOS, packet.maximum_qos);
                setPacketOptional(m_retainAvailable, packet.retain_available);
                setPacketOptional(m_maximumPacketSize, packet.maximum_packet_size);
                setPacketStringOptional(m_assignedClientIdentifier, packet.assigned_client_identifier);
                setPacketOptional(m_topicAliasMaximum, packet.topic_alias_maximum);
                setPacketStringOptional(m_reasonString, packet.reason_string);
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
                setPacketOptional(m_wildcardSubscriptionsAvailable, packet.wildcard_subscriptions_available);
                setPacketOptional(m_subscriptionIdentifiersAvailable, packet.subscription_identifiers_available);
                setPacketOptional(m_sharedSubscriptionsAvailable, packet.shared_subscriptions_available);
                setPacketOptional(m_serverKeepAliveSec, packet.server_keep_alive);
                setPacketStringOptional(m_responseInformation, packet.response_information);
                setPacketStringOptional(m_serverReference, packet.server_reference);
            }

            bool ConnAckPacket::getSessionPresent() const noexcept
            {
                return m_sessionPresent;
            }

            ConnectReasonCode ConnAckPacket::getReasonCode() const noexcept
            {
                return m_reasonCode;
            }

            const Crt::Optional<uint32_t> &ConnAckPacket::getSessionExpiryIntervalSec() const noexcept
            {
                return m_sessionExpiryIntervalSec;
            }

            const Crt::Optional<uint32_t> &ConnAckPacket::getSessionExpiryInterval() const noexcept
            {
                return getSessionExpiryIntervalSec();
            }

            const Crt::Optional<uint16_t> &ConnAckPacket::getReceiveMaximum() const noexcept
            {
                return m_receiveMaximum;
            }

            const Crt::Optional<QOS> &ConnAckPacket::getMaximumQOS() const noexcept
            {
                return m_maximumQOS;
            }

            const Crt::Optional<bool> &ConnAckPacket::getRetainAvailable() const noexcept
            {
                return m_retainAvailable;
            }

            const Crt::Optional<uint32_t> &ConnAckPacket::getMaximumPacketSize() const noexcept
            {
                return m_maximumPacketSize;
            }

            const Crt::Optional<String> &ConnAckPacket::getAssignedClientIdentifier() const noexcept
            {
                return m_assignedClientIdentifier;
            }

            const Crt::Optional<uint16_t> ConnAckPacket::getTopicAliasMaximum() const noexcept
            {
                return m_topicAliasMaximum;
            }

            const Crt::Optional<String> &ConnAckPacket::getReasonString() const noexcept
            {
                return m_reasonString;
            }

            const Vector<UserProperty> &ConnAckPacket::getUserProperty() const noexcept
            {
                return m_userProperties;
            }

            const Crt::Optional<bool> &ConnAckPacket::getWildcardSubscriptionsAvailable() const noexcept
            {
                return m_wildcardSubscriptionsAvailable;
            }

            const Crt::Optional<bool> &ConnAckPacket::getSubscriptionIdentifiersAvailable() const noexcept
            {
                return m_subscriptionIdentifiersAvailable;
            }

            const Crt::Optional<bool> &ConnAckPacket::getSharedSubscriptionsAvailable() const noexcept
            {
                return m_sharedSubscriptionsAvailable;
            }

            const Crt::Optional<uint16_t> &ConnAckPacket::getServerKeepAliveSec() const noexcept
            {
                return m_serverKeepAliveSec;
            }

            const Crt::Optional<uint16_t> &ConnAckPacket::getServerKeepAlive() const noexcept
            {
                return getServerKeepAliveSec();
            }

            const Crt::Optional<String> &ConnAckPacket::getResponseInformation() const noexcept
            {
                return m_responseInformation;
            }

            const Crt::Optional<String> &ConnAckPacket::getServerReference() const noexcept
            {
                return m_serverReference;
            }

            Subscription::Subscription(Allocator *allocator)
                : m_allocator(allocator), m_topicFilter(""), m_qos(QOS::AWS_MQTT5_QOS_AT_MOST_ONCE), m_noLocal(false),
                  m_retainAsPublished(false), m_retainHnadlingType(AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE)

            {
            }

            Subscription::Subscription(Crt::String topicFilter, Mqtt5::QOS qos, Allocator *allocator)
                : m_allocator(allocator), m_topicFilter(std::move(topicFilter)), m_qos(qos), m_noLocal(false),
                  m_retainAsPublished(false), m_retainHnadlingType(AWS_MQTT5_RHT_SEND_ON_SUBSCRIBE)
            {
            }

            Subscription &Subscription::WithTopicFilter(Crt::String topicFilter) noexcept
            {
                m_topicFilter = std::move(topicFilter);
                return *this;
            }

            Subscription &Subscription::WithQOS(Mqtt5::QOS qos) noexcept
            {
                m_qos = qos;
                return *this;
            }
            Subscription &Subscription::WithNoLocal(bool noLocal) noexcept
            {
                m_noLocal = noLocal;
                return *this;
            }
            Subscription &Subscription::WithRetain(bool retain) noexcept
            {
                return WithRetainAsPublished(retain);
            }
            Subscription &Subscription::WithRetainAsPublished(bool retain) noexcept
            {
                m_retainAsPublished = retain;
                return *this;
            }
            Subscription &Subscription::WithRetainHandlingType(RetainHandlingType retainHandlingType) noexcept
            {
                m_retainHnadlingType = retainHandlingType;
                return *this;
            }

            bool Subscription::initializeRawOptions(aws_mqtt5_subscription_view &raw_options) const noexcept
            {
                AWS_ZERO_STRUCT(raw_options);
                raw_options.topic_filter = ByteCursorFromString(m_topicFilter);
                raw_options.no_local = m_noLocal;
                raw_options.qos = m_qos;
                raw_options.retain_as_published = m_retainAsPublished;
                raw_options.retain_handling_type = m_retainHnadlingType;
                return true;
            }

            Subscription::Subscription(const Subscription &toCopy) noexcept
                : m_allocator(toCopy.m_allocator), m_topicFilter(toCopy.m_topicFilter), m_qos(toCopy.m_qos),
                  m_noLocal(toCopy.m_noLocal), m_retainAsPublished(toCopy.m_retainAsPublished),
                  m_retainHnadlingType(toCopy.m_retainHnadlingType)
            {
            }

            Subscription::Subscription(Subscription &&toMove) noexcept
                : m_allocator(toMove.m_allocator), m_topicFilter(std::move(toMove.m_topicFilter)), m_qos(toMove.m_qos),
                  m_noLocal(toMove.m_noLocal), m_retainAsPublished(toMove.m_retainAsPublished),
                  m_retainHnadlingType(toMove.m_retainHnadlingType)
            {
            }

            Subscription &Subscription::operator=(const Subscription &toCopy) noexcept
            {
                if (&toCopy != this)
                {
                    m_allocator = toCopy.m_allocator;
                    m_qos = toCopy.m_qos;
                    m_topicFilter = toCopy.m_topicFilter;
                    m_noLocal = toCopy.m_noLocal;
                    m_retainAsPublished = toCopy.m_retainAsPublished;
                    m_retainHnadlingType = toCopy.m_retainHnadlingType;
                }
                return *this;
            }

            Subscription &Subscription::operator=(Subscription &&toMove) noexcept
            {
                if (&toMove != this)
                {
                    m_allocator = toMove.m_allocator;
                    m_qos = toMove.m_qos;
                    m_topicFilter = std::move(toMove.m_topicFilter);
                    m_noLocal = toMove.m_noLocal;
                    m_retainAsPublished = toMove.m_retainAsPublished;
                    m_retainHnadlingType = toMove.m_retainHnadlingType;
                }
                return *this;
            }

            SubscribePacket::SubscribePacket(Allocator *allocator) noexcept
                : m_allocator(allocator), m_subscriptionViewStorage(nullptr), m_userPropertiesStorage(nullptr)
            {
            }

            SubscribePacket &SubscribePacket::WithUserProperties(const Vector<UserProperty> &userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            SubscribePacket &SubscribePacket::WithUserProperties(Vector<UserProperty> &&userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            SubscribePacket &SubscribePacket::WithUserProperty(UserProperty &&property) noexcept
            {
                m_userProperties.push_back(std::move(property));
                return *this;
            }

            SubscribePacket &SubscribePacket::WithSubscriptionIdentifier(uint32_t identifier) noexcept
            {
                m_subscriptionIdentifier = identifier;
                return *this;
            }

            SubscribePacket &SubscribePacket::WithSubscriptions(const Crt::Vector<Subscription> &subscriptions) noexcept
            {
                m_subscriptions = subscriptions;

                return *this;
            }

            SubscribePacket &SubscribePacket::WithSubscriptions(Vector<Subscription> &&subscriptions) noexcept
            {
                m_subscriptions = subscriptions;
                return *this;
            }

            SubscribePacket &SubscribePacket::WithSubscription(Subscription &&subscription) noexcept
            {
                m_subscriptions.push_back(subscription);
                return *this;
            }

            bool SubscribePacket::initializeRawOptions(aws_mqtt5_packet_subscribe_view &raw_options) noexcept
            {
                AWS_ZERO_STRUCT(raw_options);

                s_AllocateUnderlyingSubscription(m_subscriptionViewStorage, m_subscriptions, m_allocator);
                raw_options.subscription_count = m_subscriptions.size();
                raw_options.subscriptions = m_subscriptionViewStorage;

                s_AllocateUnderlyingUserProperties(m_userPropertiesStorage, m_userProperties, m_allocator);
                raw_options.user_properties = m_userPropertiesStorage;
                raw_options.user_property_count = m_userProperties.size();

                return true;
            }

            SubscribePacket::~SubscribePacket()
            {
                if (m_userPropertiesStorage != nullptr)
                {
                    aws_mem_release(m_allocator, m_userPropertiesStorage);
                    m_userPropertiesStorage = nullptr;
                }

                if (m_subscriptionViewStorage != nullptr)
                {
                    aws_mem_release(m_allocator, m_subscriptionViewStorage);
                    m_subscriptionViewStorage = nullptr;
                }
            }

            SubAckPacket::SubAckPacket(const aws_mqtt5_packet_suback_view &packet, Allocator * /*allocator*/) noexcept
            {
                setPacketStringOptional(m_reasonString, packet.reason_string);
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
                for (size_t i = 0; i < packet.reason_code_count; i++)
                {
                    m_reasonCodes.push_back(*(packet.reason_codes + i));
                }
            }

            const Crt::Optional<Crt::String> &SubAckPacket::getReasonString() const noexcept
            {
                return m_reasonString;
            }

            const Crt::Vector<UserProperty> &SubAckPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            const Crt::Vector<SubAckReasonCode> &SubAckPacket::getReasonCodes() const noexcept
            {
                return m_reasonCodes;
            }

            UnsubscribePacket::UnsubscribePacket(Allocator *allocator) noexcept
                : m_allocator(allocator), m_userPropertiesStorage(nullptr)
            {
                AWS_ZERO_STRUCT(m_topicFiltersList);
            }

            UnsubscribePacket &UnsubscribePacket::WithTopicFilter(Crt::String topicFilter) noexcept
            {
                m_topicFilters.push_back(std::move(topicFilter));
                return *this;
            }

            UnsubscribePacket &UnsubscribePacket::WithTopicFilters(Crt::Vector<String> topicFilters) noexcept
            {
                m_topicFilters = std::move(topicFilters);

                return *this;
            }

            UnsubscribePacket &UnsubscribePacket::WithUserProperties(
                const Vector<UserProperty> &userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            UnsubscribePacket &UnsubscribePacket::WithUserProperties(Vector<UserProperty> &&userProperties) noexcept
            {
                m_userProperties = userProperties;
                return *this;
            }

            UnsubscribePacket &UnsubscribePacket::WithUserProperty(UserProperty &&property) noexcept
            {
                m_userProperties.push_back(std::move(property));
                return *this;
            }

            bool UnsubscribePacket::initializeRawOptions(aws_mqtt5_packet_unsubscribe_view &raw_options) noexcept
            {
                AWS_ZERO_STRUCT(raw_options);

                s_AllocateStringVector(m_topicFiltersList, m_topicFilters, m_allocator);
                raw_options.topic_filters = static_cast<aws_byte_cursor *>(m_topicFiltersList.data);
                raw_options.topic_filter_count = m_topicFilters.size();

                s_AllocateUnderlyingUserProperties(m_userPropertiesStorage, m_userProperties, m_allocator);
                raw_options.user_properties = m_userPropertiesStorage;
                raw_options.user_property_count = m_userProperties.size();

                return true;
            }

            UnsubscribePacket::~UnsubscribePacket()
            {
                aws_array_list_clean_up(&m_topicFiltersList);
                AWS_ZERO_STRUCT(m_topicFiltersList);

                if (m_userPropertiesStorage != nullptr)
                {
                    aws_mem_release(m_allocator, m_userPropertiesStorage);
                    m_userPropertiesStorage = nullptr;
                }
            }

            UnSubAckPacket::UnSubAckPacket(const aws_mqtt5_packet_unsuback_view &packet, Allocator *allocator) noexcept
            {
                (void)allocator;

                setPacketStringOptional(m_reasonString, packet.reason_string);

                for (size_t i = 0; i < packet.reason_code_count; i++)
                {
                    m_reasonCodes.push_back(*(packet.reason_codes + i));
                }
                setUserProperties(m_userProperties, packet.user_properties, packet.user_property_count);
            }

            const Crt::Optional<Crt::String> &UnSubAckPacket::getReasonString() const noexcept
            {
                return m_reasonString;
            }

            const Crt::Vector<UserProperty> &UnSubAckPacket::getUserProperties() const noexcept
            {
                return m_userProperties;
            }

            const Crt::Vector<UnSubAckReasonCode> &UnSubAckPacket::getReasonCodes() const noexcept
            {
                return m_reasonCodes;
            }

            NegotiatedSettings::NegotiatedSettings(
                const aws_mqtt5_negotiated_settings &negotiated_settings,
                Allocator *allocator) noexcept
            {
                (void)allocator;

                m_maximumQOS = negotiated_settings.maximum_qos;
                m_sessionExpiryIntervalSec = negotiated_settings.session_expiry_interval;
                m_receiveMaximumFromServer = negotiated_settings.receive_maximum_from_server;

                m_maximumPacketSizeBytes = negotiated_settings.maximum_packet_size_to_server;
                m_topicAliasMaximumToServer = negotiated_settings.topic_alias_maximum_to_server;
                m_topicAliasMaximumToClient = negotiated_settings.topic_alias_maximum_to_client;
                m_serverKeepAliveSec = negotiated_settings.server_keep_alive;

                m_retainAvailable = negotiated_settings.retain_available;
                m_wildcardSubscriptionsAvailable = negotiated_settings.wildcard_subscriptions_available;
                m_subscriptionIdentifiersAvailable = negotiated_settings.subscription_identifiers_available;
                m_sharedSubscriptionsAvailable = negotiated_settings.shared_subscriptions_available;
                m_rejoinedSession = negotiated_settings.rejoined_session;

                m_clientId = Crt::String(
                    (const char *)negotiated_settings.client_id_storage.buffer,
                    negotiated_settings.client_id_storage.len);
            }

            Mqtt5::QOS NegotiatedSettings::getMaximumQOS() const noexcept
            {
                return m_maximumQOS;
            }

            uint32_t NegotiatedSettings::getSessionExpiryIntervalSec() const noexcept
            {
                return m_sessionExpiryIntervalSec;
            }

            uint16_t NegotiatedSettings::getReceiveMaximumFromServer() const noexcept
            {
                return m_receiveMaximumFromServer;
            }

            uint32_t NegotiatedSettings::getMaximumPacketSizeBytes() const noexcept
            {
                return getMaximumPacketSizeToServer();
            }

            uint32_t NegotiatedSettings::getMaximumPacketSizeToServer() const noexcept
            {
                return m_maximumPacketSizeBytes;
            }

            uint16_t NegotiatedSettings::getTopicAliasMaximumToServer() const noexcept
            {
                return m_topicAliasMaximumToServer;
            }

            uint16_t NegotiatedSettings::getTopicAliasMaximumToClient() const noexcept
            {
                return m_topicAliasMaximumToClient;
            }

            uint16_t NegotiatedSettings::getServerKeepAliveSec() const noexcept
            {
                return m_serverKeepAliveSec;
            }

            uint16_t NegotiatedSettings::getServerKeepAlive() const noexcept
            {
                return getServerKeepAliveSec();
            }

            bool NegotiatedSettings::getRetainAvailable() const noexcept
            {
                return m_retainAvailable;
            }

            bool NegotiatedSettings::getWildcardSubscriptionsAvailable() const noexcept
            {
                return m_wildcardSubscriptionsAvailable;
            }

            bool NegotiatedSettings::getSubscriptionIdentifiersAvailable() const noexcept
            {
                return m_subscriptionIdentifiersAvailable;
            }

            bool NegotiatedSettings::getSharedSubscriptionsAvailable() const noexcept
            {
                return m_sharedSubscriptionsAvailable;
            }

            bool NegotiatedSettings::getRejoinedSession() const noexcept
            {
                return m_rejoinedSession;
            }

            const Crt::String &NegotiatedSettings::getClientId() const noexcept
            {
                return m_clientId;
            }

            PublishResult::PublishResult() : m_ack(nullptr), m_errorCode(0) {}

            PublishResult::PublishResult(std::shared_ptr<PubAckPacket> puback) : m_errorCode(0)
            {
                m_ack = puback;
            }

            PublishResult::PublishResult(int error) : m_ack(nullptr), m_errorCode(error) {}

            PublishResult::~PublishResult() noexcept
            {
                m_ack.reset();
            }

        } // namespace Mqtt5
    } // namespace Crt
} // namespace Aws
