/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/event/EventMessage.h>
#include <aws/core/utils/HashingUtils.h>
#include <algorithm>
#include <iterator>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            const char EVENT_TYPE_HEADER[] = ":event-type";
            const char CONTENT_TYPE_HEADER[] = ":content-type";
            const char MESSAGE_TYPE_HEADER[] = ":message-type";
            const char ERROR_CODE_HEADER[] = ":error-code";
            const char ERROR_MESSAGE_HEADER[] = ":error-message";
            const char EXCEPTION_TYPE_HEADER[] = ":exception-type";

            static const int EVENT_HASH = HashingUtils::HashString("event");
            static const int ERROR_HASH = HashingUtils::HashString("error");
            static const int EXCEPTION_HASH = HashingUtils::HashString("exception");

            static const int CONTENT_TYPE_APPLICATION_OCTET_STREAM_HASH = HashingUtils::HashString("application/octet-stream");
            static const int CONTENT_TYPE_APPLICATION_JSON_HASH = HashingUtils::HashString("application/json");
            static const int CONTENT_TYPE_TEXT_PLAIN_HASH = HashingUtils::HashString("text/plain");

            Message::MessageType Message::GetMessageTypeForName(const Aws::String& name)
            {
                int hashCode = Aws::Utils::HashingUtils::HashString(name.c_str());
                if (hashCode == EVENT_HASH)
                {
                    return MessageType::EVENT;
                }
                else if (hashCode == ERROR_HASH)
                {
                    return MessageType::REQUEST_LEVEL_ERROR;
                }
                else if (hashCode == EXCEPTION_HASH)
                {
                    return MessageType::REQUEST_LEVEL_EXCEPTION;
                }
                else
                {
                    return MessageType::UNKNOWN;
                }
            }

            Aws::String Message::GetNameForMessageType(MessageType value)
            {
                switch (value)
                {
                case MessageType::EVENT:
                    return "event";
                case MessageType::REQUEST_LEVEL_ERROR:
                    return "error";
                case MessageType::REQUEST_LEVEL_EXCEPTION:
                    return "exception";
                default:
                    return "unknown";
                }
            }

            Message::ContentType Message::GetContentTypeForName(const Aws::String& name)
            {
                int hashCode = Aws::Utils::HashingUtils::HashString(name.c_str());
                if (hashCode == CONTENT_TYPE_APPLICATION_OCTET_STREAM_HASH)
                {
                    return ContentType::APPLICATION_OCTET_STREAM;
                }
                else if (hashCode == CONTENT_TYPE_APPLICATION_JSON_HASH)
                {
                    return ContentType::APPLICATION_JSON;
                }
                else if (hashCode == CONTENT_TYPE_TEXT_PLAIN_HASH)
                {
                    return ContentType::TEXT_PLAIN;
                }
                else
                {
                    return ContentType::UNKNOWN;
                }
            }

            Aws::String Message::GetNameForContentType(ContentType value)
            {
                switch (value)
                {
                case ContentType::APPLICATION_OCTET_STREAM:
                    return "application/octet-stream";
                case ContentType::APPLICATION_JSON:
                    return "application/json";
                case ContentType::TEXT_PLAIN:
                    return "text/plain";
                default:
                    return "unknown";
                }
            }

            void Message::Reset()
            {
                m_totalLength = 0;
                m_headersLength = 0;
                m_payloadLength = 0;

                m_eventHeaders.clear();
                m_eventPayload.clear();
            }

            void Message::WriteEventPayload(const unsigned char* data, size_t length)
            {
                std::copy(data, data + length, std::back_inserter(m_eventPayload));
            }

            void Message::WriteEventPayload(const Aws::Vector<unsigned char>& bits)
            {
                std::copy(bits.cbegin(), bits.cend(), std::back_inserter(m_eventPayload));
            }

            void Message::WriteEventPayload(const Aws::String& bits)
            {
                std::copy(bits.cbegin(), bits.cend(), std::back_inserter(m_eventPayload));
            }

        } // namespace Event
    } // namespace Utils
} // namespace Aws

