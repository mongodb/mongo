/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/event/EventHeader.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            extern AWS_CORE_API const char EVENT_TYPE_HEADER[];
            extern AWS_CORE_API const char CONTENT_TYPE_HEADER[];
            extern AWS_CORE_API const char MESSAGE_TYPE_HEADER[];
            extern AWS_CORE_API const char ERROR_CODE_HEADER[];
            extern AWS_CORE_API const char ERROR_MESSAGE_HEADER[];
            extern AWS_CORE_API const char EXCEPTION_TYPE_HEADER[];

            /**
             * A typical message in event stream consists of two parts: Prelude and Data, as well as the prelude CRC and message CRC.
             * Prelude consists of total byte length and headers byte length.
             * Data consists of headers and payload.
             */
            class AWS_CORE_API Message
            {
            public:
                enum class MessageType
                {
                    UNKNOWN,
                    EVENT,
                    REQUEST_LEVEL_ERROR,
                    REQUEST_LEVEL_EXCEPTION
                };

                enum class ContentType
                {
                    UNKNOWN,
                    APPLICATION_OCTET_STREAM,
                    APPLICATION_JSON,
                    TEXT_PLAIN
                };

                static MessageType GetMessageTypeForName(const Aws::String& name);
                static Aws::String GetNameForMessageType(MessageType value);

                static ContentType GetContentTypeForName(const Aws::String& name);
                static Aws::String GetNameForContentType(ContentType value);


                /**
                 * Clean up the message, including the metadata, headers and payload received.
                 */
                void Reset();

                /**
                 * Get/set the total length of this message: prelude(8 bytes) + prelude CRC(4 bytes) + Data(headers length + payload length) + message CRC(4 bytes).
                 */
                inline void SetTotalLength(size_t length)
                {
                    m_totalLength = length;
                    m_eventPayload.reserve(length);
                }

                inline size_t GetTotalLength() const { return m_totalLength; }

                /**
                 * Get/set the length of the headers.
                 * Each header consists of: header name byte-length(1 byte) + header name + header value type(1 byte) + header value.
                 */
                inline void SetHeadersLength(size_t length) { m_headersLength = length; }
                inline size_t GetHeadersLength() const { return m_headersLength; }

                /**
                 * Get/set the length of payload.
                 */
                inline void SetPayloadLength(size_t length) { m_payloadLength = length; }
                inline size_t GetPayloadLength() const { return m_payloadLength; }

                /**
                 * Set/get event headers.
                 */
                inline void InsertEventHeader(const Aws::String& headerName, const EventHeaderValue& eventHeaderValue)
                {
                    m_eventHeaders.emplace(Aws::Utils::Event::EventHeaderValuePair(headerName, eventHeaderValue));
                }

                inline const Aws::Utils::Event::EventHeaderValueCollection& GetEventHeaders() const { return m_eventHeaders; }

                /**
                 * Set event payload.
                 */
                void WriteEventPayload(const unsigned char* data, size_t length);
                void WriteEventPayload(const Aws::Vector<unsigned char>& bits);
                void WriteEventPayload(const Aws::String& bits);
                /**
                 * Get the byte array of the payload with transferring ownership.
                 */
                Aws::Vector<unsigned char>&& GetEventPayloadWithOwnership() { return std::move(m_eventPayload); }
                const Aws::Vector<unsigned char>& GetEventPayload() const { return m_eventPayload; }
                Aws::Vector<unsigned char>& GetEventPayload() { return m_eventPayload; }
                /**
                 * Convert byte array of the payload to string without transferring ownership.
                 */
                inline Aws::String GetEventPayloadAsString() { return Aws::String(m_eventPayload.begin(), m_eventPayload.end()); }

            private:
                size_t m_totalLength = 0;
                size_t m_headersLength = 0;
                size_t m_payloadLength = 0;

                Aws::Utils::Event::EventHeaderValueCollection m_eventHeaders;
                Aws::Vector<unsigned char> m_eventPayload;
            };

        }
    }
}
