/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UUID.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/event-stream/event_stream.h>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            static const char CLASS_TAG[] = "EventHeader";
            /**
             * Interface of the header value of a message in event stream.
             * Each type of header value should have it's own associated derived class based on this class.
             */
            class AWS_CORE_API EventHeaderValue
            {
            public:
                enum class EventHeaderType
                {
                    BOOL_TRUE = 0,
                    BOOL_FALSE,
                    BYTE,
                    INT16,
                    INT32,
                    INT64,
                    BYTE_BUF,
                    STRING,
                    /* 64 bit integer (millis since epoch) */
                    TIMESTAMP,
                    UUID,
                    UNKNOWN
                };

                EventHeaderValue() : m_eventHeaderType(EventHeaderType::UNKNOWN), m_eventHeaderStaticValue({0}) {}

                EventHeaderValue(aws_event_stream_header_value_pair* header) :
                    m_eventHeaderType(static_cast<EventHeaderType>(header->header_value_type)),
                    m_eventHeaderStaticValue({0})
                {
                    switch (m_eventHeaderType)
                    {
                    case EventHeaderType::BOOL_TRUE:
                    case EventHeaderType::BOOL_FALSE:
                        m_eventHeaderStaticValue.boolValue = aws_event_stream_header_value_as_bool(header) != 0;
                        break;
                    case EventHeaderType::BYTE:
                        m_eventHeaderStaticValue.byteValue = aws_event_stream_header_value_as_byte(header);
                        break;
                    case EventHeaderType::INT16:
                        m_eventHeaderStaticValue.int16Value = aws_event_stream_header_value_as_int16(header);
                        break;
                    case EventHeaderType::INT32:
                        m_eventHeaderStaticValue.int32Value = aws_event_stream_header_value_as_int32(header);
                        break;
                    case EventHeaderType::INT64:
                        m_eventHeaderStaticValue.int64Value = aws_event_stream_header_value_as_int64(header);
                        break;
                    case EventHeaderType::BYTE_BUF:
                        m_eventHeaderVariableLengthValue = ByteBuffer(static_cast<uint8_t*>(aws_event_stream_header_value_as_bytebuf(header).buffer), header->header_value_len);
                        break;
                    case EventHeaderType::STRING:
                        m_eventHeaderVariableLengthValue = ByteBuffer(static_cast<uint8_t*>(aws_event_stream_header_value_as_string(header).buffer), header->header_value_len);
                        break;
                    case EventHeaderType::TIMESTAMP:
                        m_eventHeaderStaticValue.timestampValue = aws_event_stream_header_value_as_timestamp(header);
                        break;
                    case EventHeaderType::UUID:
                        assert(header->header_value_len == 16u);
                        m_eventHeaderVariableLengthValue = ByteBuffer(static_cast<uint8_t*>(aws_event_stream_header_value_as_uuid(header).buffer), header->header_value_len);
                        break;
                    default:
                        AWS_LOG_ERROR(CLASS_TAG, "Encountered unknown type of header.");
                        break;
                    }
                };

                EventHeaderValue(const Aws::String& s) :
                    m_eventHeaderType(EventHeaderType::STRING),
                    m_eventHeaderVariableLengthValue(reinterpret_cast<const uint8_t*>(s.data()), s.length()),
                    m_eventHeaderStaticValue({0})
                {
                }

                EventHeaderValue(const ByteBuffer& bb) :
                    m_eventHeaderType(EventHeaderType::BYTE_BUF),
                    m_eventHeaderVariableLengthValue(bb),
                    m_eventHeaderStaticValue({0})
                {
                }

                EventHeaderValue(ByteBuffer&& bb) :
                    m_eventHeaderType(EventHeaderType::BYTE_BUF),
                    m_eventHeaderVariableLengthValue(std::move(bb)),
                    m_eventHeaderStaticValue({0})
                {
                }


                explicit EventHeaderValue(unsigned char byte) :
                    m_eventHeaderType(EventHeaderType::BYTE),
                    m_eventHeaderStaticValue({0})
                {
                    m_eventHeaderStaticValue.byteValue = byte;
                }

                explicit EventHeaderValue(bool b) :
                    m_eventHeaderType(b ? EventHeaderType::BOOL_TRUE : EventHeaderType::BOOL_FALSE),
                    m_eventHeaderStaticValue({0})
                {
                    m_eventHeaderStaticValue.boolValue = b;
                }

                explicit EventHeaderValue(int16_t n) :
                    m_eventHeaderType(EventHeaderType::INT16),
                    m_eventHeaderStaticValue({0})
                {
                    m_eventHeaderStaticValue.int16Value = n;
                }

                explicit EventHeaderValue(int32_t n) :
                    m_eventHeaderType(EventHeaderType::INT32),
                    m_eventHeaderStaticValue({0})
                {
                    m_eventHeaderStaticValue.int32Value = n;
                }

                explicit EventHeaderValue(int64_t n, EventHeaderType type = EventHeaderType::INT64) :
                    m_eventHeaderType(type),
                    m_eventHeaderStaticValue({0})
                {
                    if (type == EventHeaderType::TIMESTAMP)
                    {
                        m_eventHeaderStaticValue.timestampValue = n;
                    }
                    else
                    {
                        m_eventHeaderStaticValue.int64Value = n;
                    }
                }

                EventHeaderType GetType() const { return m_eventHeaderType; }


                static EventHeaderType GetEventHeaderTypeForName(const Aws::String& name);
                static Aws::String GetNameForEventHeaderType(EventHeaderType value);

                /**
                 * Get header value as boolean.
                 * Log error if derived class doesn't override this function.
                 */
                inline bool GetEventHeaderValueAsBoolean() const
                {
                    assert(m_eventHeaderType == EventHeaderType::BOOL_TRUE || m_eventHeaderType == EventHeaderType::BOOL_FALSE);
                    if (m_eventHeaderType != EventHeaderType::BOOL_TRUE && m_eventHeaderType != EventHeaderType::BOOL_FALSE)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is TRUE or FALSE, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return false;
                    }
                    return m_eventHeaderStaticValue.boolValue;
                }

                /**
                 * Get header value as byte.
                 * Log error if derived class doesn't override this function.
                 */
                inline uint8_t GetEventHeaderValueAsByte() const
                {
                    assert(m_eventHeaderType == EventHeaderType::BYTE);
                    if (m_eventHeaderType != EventHeaderType::BYTE)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is BYTE, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return static_cast<uint8_t>(0);
                    }
                    return m_eventHeaderStaticValue.byteValue;
                }

                /**
                 * Get header value as 16 bit integer.
                 * Log error if derived class doesn't override this function.
                 */
                inline int16_t GetEventHeaderValueAsInt16() const
                {
                    assert(m_eventHeaderType == EventHeaderType::INT16);
                    if (m_eventHeaderType != EventHeaderType::INT16)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is INT16, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return static_cast<int16_t>(0);
                    }
                    return m_eventHeaderStaticValue.int16Value;
                }

                /**
                 * Get header value as 32 bit integer.
                 * Log error if derived class doesn't override this function.
                 */
                inline int32_t GetEventHeaderValueAsInt32() const
                {
                    assert(m_eventHeaderType == EventHeaderType::INT32);
                    if (m_eventHeaderType != EventHeaderType::INT32)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is INT32, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return static_cast<int32_t>(0);
                    }
                    return m_eventHeaderStaticValue.int32Value;
                }

                /**
                 * Get header value as 64 bit integer.
                 * Log error if derived class doesn't override this function.
                 */
                inline int64_t GetEventHeaderValueAsInt64() const
                {
                    assert(m_eventHeaderType == EventHeaderType::INT64);
                    if (m_eventHeaderType != EventHeaderType::INT64)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is INT64, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return static_cast<uint64_t>(0);
                    }
                    return m_eventHeaderStaticValue.int64Value;
                }

                /**
                 * Get header value as ByteBuffer.
                 * Log error if derived class doesn't override this function.
                 */
                inline ByteBuffer GetEventHeaderValueAsBytebuf() const
                {
                    assert(m_eventHeaderType == EventHeaderType::BYTE_BUF);
                    if (m_eventHeaderType != EventHeaderType::BYTE_BUF)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is BYTE_BUF, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return ByteBuffer();
                    }
                    return m_eventHeaderVariableLengthValue;
                }

                /**
                 * Get header value as String.
                 * Log error if derived class doesn't override this function.
                 */
                inline Aws::String GetEventHeaderValueAsString() const
                {
                    assert(m_eventHeaderType == EventHeaderType::STRING);
                    if (m_eventHeaderType != EventHeaderType::STRING)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is STRING, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return {};
                    }
                    return Aws::String(reinterpret_cast<char*>(m_eventHeaderVariableLengthValue.GetUnderlyingData()), m_eventHeaderVariableLengthValue.GetLength());
                }

                /**
                 * Get header value as timestamp in 64 bit integer.
                 * Log error if derived class doesn't override this function.
                 */
                inline int64_t GetEventHeaderValueAsTimestamp() const
                {
                    assert(m_eventHeaderType == EventHeaderType::TIMESTAMP);
                    if (m_eventHeaderType != EventHeaderType::TIMESTAMP)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is TIMESTAMP, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        return static_cast<int64_t>(0);
                    }
                    return m_eventHeaderStaticValue.timestampValue;
                }

                /**
                 * Get header value as UUID.
                 * Log error if derived class doesn't override this function.
                 */
                inline Aws::Utils::UUID GetEventHeaderValueAsUuid() const
                {
                    assert(m_eventHeaderType == EventHeaderType::UUID);
                    assert(m_eventHeaderVariableLengthValue.GetLength() == 16u);
                    if (m_eventHeaderType != EventHeaderType::UUID)
                    {
                        AWS_LOGSTREAM_ERROR(CLASS_TAG, "Expected event header type is UUID, but encountered " << GetNameForEventHeaderType(m_eventHeaderType));
                        char uuid[32] = {0};
                        return Aws::Utils::UUID(uuid);
                    }
                    return Aws::Utils::UUID(m_eventHeaderVariableLengthValue.GetUnderlyingData());
                }

                inline const ByteBuffer& GetUnderlyingBuffer() const
                {
                    return m_eventHeaderVariableLengthValue;
                }

                inline Aws::String ToString() const
                {
                    switch (m_eventHeaderType)
                    {
                        case EventHeaderType::BOOL_TRUE:
                        case EventHeaderType::BOOL_FALSE:
                            return Utils::StringUtils::to_string(GetEventHeaderValueAsBoolean());
                        case EventHeaderType::BYTE:
                            return Utils::StringUtils::to_string(GetEventHeaderValueAsByte());
                        case EventHeaderType::INT16:
                            return Utils::StringUtils::to_string(GetEventHeaderValueAsInt16());
                        case EventHeaderType::INT32:
                            return Utils::StringUtils::to_string(GetEventHeaderValueAsInt32());
                        case EventHeaderType::INT64:
                            return Utils::StringUtils::to_string(GetEventHeaderValueAsInt64());
                        case EventHeaderType::BYTE_BUF:
                            return Aws::String(reinterpret_cast<char*>(GetEventHeaderValueAsBytebuf().GetUnderlyingData()), GetEventHeaderValueAsBytebuf().GetLength());
                        case EventHeaderType::STRING:
                            return GetEventHeaderValueAsString();
                        case EventHeaderType::TIMESTAMP:
                            return Aws::Utils::DateTime(GetEventHeaderValueAsTimestamp()).ToGmtString(Aws::Utils::DateFormat::RFC822);
                        case EventHeaderType::UUID:
                            return GetEventHeaderValueAsUuid();
                        case EventHeaderType::UNKNOWN:
                        default:
                            AWS_LOGSTREAM_ERROR(CLASS_TAG, "Cannot transform EventHeader value to string: type is unknown");
                            return {};
                    }
                }

            private:
                EventHeaderType m_eventHeaderType;
                ByteBuffer m_eventHeaderVariableLengthValue;
                union
                {
                    int64_t timestampValue;
                    int64_t int64Value;
                    int32_t int32Value;
                    int16_t int16Value;
                    uint8_t byteValue;
                    bool boolValue;
                } m_eventHeaderStaticValue;
            };

            typedef std::pair<Aws::String, EventHeaderValue> EventHeaderValuePair;
            typedef Aws::Map<Aws::String, EventHeaderValue> EventHeaderValueCollection;
        }
    }
}
