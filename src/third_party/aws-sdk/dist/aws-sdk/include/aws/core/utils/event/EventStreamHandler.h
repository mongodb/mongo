/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/utils/event/EventHeader.h>
#include <aws/core/utils/event/EventMessage.h>
#include <aws/core/utils/event/EventStreamErrors.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            enum class InitialResponseType
            {
                ON_EVENT,
                ON_RESPONSE
            };

            /**
             * Handler of event stream.
             * Includes context and callback function while scanning the event stream.
             */
            class AWS_CORE_API EventStreamHandler
            {
            public:
                EventStreamHandler() :
                    m_failure(false), m_internalError(EventStreamErrors::EVENT_STREAM_NO_ERROR), m_headersBytesReceived(0), m_payloadBytesReceived(0), m_message()
                {}

                virtual ~EventStreamHandler() = default;
            
                /**
                 * Whether or not flow handler is in a good state. Return false if handler encounters errors.
                 */
                inline operator bool() const { return !m_failure; }

                /**
                 * Fail the handler from outside.
                 */
                inline void SetFailure() { m_failure = true; }

                /**
                 * Clean up current bytes of data received, as well as the latest message.
                 */
                inline virtual void Reset()
                {
                    m_failure = false;
                    m_internalError = EventStreamErrors::EVENT_STREAM_NO_ERROR;
                    m_headersBytesReceived = 0;
                    m_payloadBytesReceived = 0;

                    m_message.Reset();
                }

                /**
                 * Set internal Event Stream Errors, which is associated with errors in aws-c-event-stream library.
                 */
                inline void SetInternalError(int errorCode = 0)
                {
                    m_internalError = static_cast<EventStreamErrors>(errorCode);
                }

                /**
                 * Get internal Event Stream Errors.
                 */
                inline EventStreamErrors GetInternalError()
                {
                    return m_internalError;
                }

                /**
                 * The message is considered to completed with the following scenarios:
                 * 1. Message doesn't have headers or payloads. Or
                 * 2. Message has headers but doesn't have payloads. Or
                 * 3. Message has both headers and payloads.
                 */
                inline virtual bool IsMessageCompleted()
                {
                    return m_message.GetHeadersLength() == m_headersBytesReceived && m_message.GetPayloadLength() == m_payloadBytesReceived;
                }

                /**
                 * Set message metadata, including total message length, headers length and payload length.
                 */
                inline virtual void SetMessageMetadata(size_t totalLength, size_t headersLength, size_t payloadLength)
                {
                    m_message.SetTotalLength(totalLength);
                    m_message.SetHeadersLength(headersLength);
                    m_message.SetPayloadLength(payloadLength);
                    assert(totalLength == 12/*prelude length*/ + headersLength + payloadLength + 4/*message crc length*/);
                    if (totalLength != headersLength + payloadLength + 16)
                    {
                        AWS_LOG_WARN("EventStreamHandler", "Message total length mismatch.");
                    }
                }

                /**
                 * Write data to underlying stream, and update payload bytes received.
                 */
                inline virtual void WriteMessageEventPayload(const unsigned char* data, size_t dataLength)
                {
                    m_message.WriteEventPayload(data, dataLength);
                    m_payloadBytesReceived += dataLength;
                }
                
                /**
                 * Get underlying byte array of the message just received.
                 */
                inline virtual Aws::Vector<unsigned char>&& GetEventPayloadWithOwnership() { return m_message.GetEventPayloadWithOwnership(); }

                /**
                 * Convert underlying byte array to string without transferring ownership.
                 */
                inline virtual Aws::String GetEventPayloadAsString() { return m_message.GetEventPayloadAsString(); }

                /**
                 * Insert event header to a underlying event header value map, and update headers bytes received.
                 */
                inline virtual void InsertMessageEventHeader(const String& eventHeaderName, size_t eventHeaderLength, const Aws::Utils::Event::EventHeaderValue& eventHeaderValue)
                {
                    m_message.InsertEventHeader(eventHeaderName, eventHeaderValue);
                    m_headersBytesReceived += eventHeaderLength;
                }

                inline virtual const Aws::Utils::Event::EventHeaderValueCollection& GetEventHeaders() { return m_message.GetEventHeaders(); }

                inline virtual const Http::HeaderValueCollection GetEventHeadersAsHttpHeaders() const
                {
                    Http::HeaderValueCollection output;
                    using SrcT = Aws::Utils::Event::EventHeaderValueCollection::value_type;
                    using DstT = Http::HeaderValueCollection::value_type;
                    std::transform(m_message.GetEventHeaders().cbegin(), m_message.GetEventHeaders().cend(),
                                   std::inserter(output, output.end()),
                                   [](const SrcT& src)
                                   {
                                       return DstT(src.first, src.second.ToString());
                                   });
                    return output;
                }

                /**
                 * Entry point of all callback functions.
                 * Will trigger associated functions based on m_message.
                 */ 
                virtual void OnEvent() = 0;

            private:
                bool m_failure;
                EventStreamErrors m_internalError;
                size_t m_headersBytesReceived;
                size_t m_payloadBytesReceived;
                Aws::Utils::Event::Message m_message;
            };
        }
    }
}
