/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/core/utils/event/EventHeader.h>
#include <aws/core/utils/event/EventMessage.h>
#include <aws/core/utils/event/EventStreamDecoder.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/UnreferencedParam.h>
#include <aws/core/utils/memory/AWSMemory.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            static const char EVENT_STREAM_DECODER_CLASS_TAG[] = "Aws::Utils::Event::EventStreamDecoder";

            EventStreamDecoder::EventStreamDecoder(EventStreamHandler* handler) : m_eventStreamHandler(handler)
            {
                aws_event_stream_streaming_decoder_init(&m_decoder,
                    get_aws_allocator(),
                    onPayloadSegment,
                    onPreludeReceived,
                    onHeaderReceived,
                    onError,
                    (void*)handler);
            }

            EventStreamDecoder::~EventStreamDecoder()
            {
                aws_event_stream_streaming_decoder_clean_up(&m_decoder);
            }

            void EventStreamDecoder::Pump(const ByteBuffer& data)
            {
                Pump(data, data.GetLength());
            }

            void EventStreamDecoder::Pump(const ByteBuffer& data, size_t length)
            {
                aws_byte_buf dataBuf = aws_byte_buf_from_array(static_cast<uint8_t*>(data.GetUnderlyingData()), length);
                aws_event_stream_streaming_decoder_pump(&m_decoder, &dataBuf);
            }

            void EventStreamDecoder::Reset()
            {
                m_eventStreamHandler->Reset();
            }

            void EventStreamDecoder::ResetEventStreamHandler(EventStreamHandler* handler)
            {
                aws_event_stream_streaming_decoder_init(&m_decoder, get_aws_allocator(),
                    onPayloadSegment,
                    onPreludeReceived,
                    onHeaderReceived,
                    onError,
                    reinterpret_cast<void *>(handler));
            }

            void EventStreamDecoder::onPayloadSegment(
                aws_event_stream_streaming_decoder* decoder,
                aws_byte_buf* payload,
                int8_t isFinalSegment,
                void* context)
            {
                AWS_UNREFERENCED_PARAM(decoder);
                auto handler = static_cast<EventStreamHandler*>(context);
                assert(handler);
                if (!handler)
                {
                    AWS_LOGSTREAM_ERROR(EVENT_STREAM_DECODER_CLASS_TAG, "Payload received, but handler is null.");
                    return;
                }
                handler->WriteMessageEventPayload(static_cast<unsigned char*>(payload->buffer), payload->len);

                // Complete payload received
                if (isFinalSegment == 1)
                {
                    assert(handler->IsMessageCompleted());
                    handler->OnEvent();
                    handler->Reset();
                }
            }

            void EventStreamDecoder::onPreludeReceived(
                aws_event_stream_streaming_decoder* decoder,
                aws_event_stream_message_prelude* prelude,
                void* context)
            {
                AWS_UNREFERENCED_PARAM(decoder);
                auto handler = static_cast<EventStreamHandler*>(context);
                handler->Reset();
                
                //Encounter internal error in prelude received.
                //This error will be handled by OnError callback function later.
                if (prelude->total_len < prelude->headers_len + 16)
                {
                    return;
                }
                handler->SetMessageMetadata(prelude->total_len, prelude->headers_len,
                    prelude->total_len - prelude->headers_len - 4/*total byte-length*/ - 4/*headers byte-length*/ - 4/*prelude crc*/ - 4/*message crc*/);
                AWS_LOGSTREAM_TRACE(EVENT_STREAM_DECODER_CLASS_TAG, "Message received, the expected length of the message is: " << prelude->total_len <<
                                                                    " bytes, and the expected length of the header is: " << prelude->headers_len << " bytes");

                //Handle empty message 
                //if (handler->m_message.GetHeadersLength() == 0 && handler->m_message.GetPayloadLength() == 0)
                if (handler->IsMessageCompleted())
                {
                    handler->OnEvent();
                    handler->Reset();
                }
            }

            void EventStreamDecoder::onHeaderReceived(
                aws_event_stream_streaming_decoder* decoder,
                aws_event_stream_message_prelude* prelude,
                aws_event_stream_header_value_pair* header,
                void* context)
            {
                AWS_UNREFERENCED_PARAM(decoder);
                AWS_UNREFERENCED_PARAM(prelude);
                auto handler = static_cast<EventStreamHandler*>(context);
                assert(handler);
                if (!handler)
                {
                    AWS_LOGSTREAM_ERROR(EVENT_STREAM_DECODER_CLASS_TAG, "Header received, but handler is null.");
                    return;
                }

                // The length of a header = 1 byte (to represent the length of header name) + length of header name + 1 byte (to represent header type)
                //                          + 2 bytes (to represent length of header value) + length of header value
                handler->InsertMessageEventHeader(Aws::String(header->header_name, header->header_name_len),
                    1 + header->header_name_len + 1 + 2 + header->header_value_len, EventHeaderValue(header));

                // Handle messages only have headers, but without payload.
                //if (handler->m_message.GetHeadersLength() == handler->m_headersBytesReceived() && handler->m_message.GetPayloadLength() == 0)
                if (handler->IsMessageCompleted())
                {
                    handler->OnEvent();
                    handler->Reset();
                }
            }

            void EventStreamDecoder::onError(
                aws_event_stream_streaming_decoder* decoder,
                aws_event_stream_message_prelude* prelude,
                int error_code,
                const char* message,
                void* context)
            {
                AWS_UNREFERENCED_PARAM(decoder);
                AWS_UNREFERENCED_PARAM(prelude);
                auto handler = static_cast<EventStreamHandler*>(context);
                handler->SetFailure();
                handler->SetInternalError(error_code);
                handler->WriteMessageEventPayload(reinterpret_cast<const unsigned char*>(message), strlen(message));
                handler->OnEvent();
            }
        } // namespace Event
    } // namespace Utils
} // namespace Aws

