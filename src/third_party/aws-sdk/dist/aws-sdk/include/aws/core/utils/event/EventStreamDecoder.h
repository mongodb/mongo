/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/event/EventStreamHandler.h>
#include <aws/event-stream/event_stream.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            class AWS_CORE_API EventStreamDecoder
            {
            public:
                EventStreamDecoder(EventStreamHandler* handler);
                ~EventStreamDecoder();

                /**
                 * Whether or not the decoder is in good state. Return false if the decoder encounters errors.
                 */
                inline explicit operator bool() const { return *m_eventStreamHandler; }

                /**
                 * A wrapper of aws_event_stream_streaming_decoder_pump in aws-c-event-stream.
                 * Pass data to the underlying decoder.
                 */
                void Pump(const ByteBuffer& data);
                void Pump(const ByteBuffer& data, size_t length);

                /**
                 * Reset decoder and it's handler.
                 */
                void Reset();

                /**
                 * Reset event stream handler of the decoder
                 */
                void ResetEventStreamHandler(EventStreamHandler* handler);

            protected:
                /**
                 * Callback function invoked when payload data has been received.
                 * @param decoder The underlying decoder defined in the aws-c-event-stream.
                 * @param payload The payload data received, it doesn't belong to you, make a copy if necessary.
                 * @param isFinalSegment A flag indicates the current data is the last payload buffer for that message if it equals to 1.
                 * @param context A context pointer, will cast it to a pointer of flow handler.
                 */
                static void onPayloadSegment(
                    aws_event_stream_streaming_decoder* decoder,
                    aws_byte_buf* payload,
                    int8_t isFinalSegment,
                    void* context);

                /**
                 * Callback function invoked when a new message has arrived.
                 * @param decoder The underlying decoder defined in the aws-c-event-stream.
                 * @param prelude The metadata of the message, including total message length and header length.
                 * @param context A context pointer, will cast it to a pointer of flow handler.
                 */
                static void onPreludeReceived(
                    aws_event_stream_streaming_decoder* decoder,
                    aws_event_stream_message_prelude* prelude,
                    void* context);

                /**
                 * Callback function invoked when a header is encountered.
                 * @param decoder The underlying decoder defined in the aws-c-event-stream.
                 * @param prelude The metadata of the message, including total message length and header length.
                 * @param header A header of the message.
                 * @param context A context pointer, will cast it to a pointer of flow handler.
                 */
                static void onHeaderReceived(
                    aws_event_stream_streaming_decoder* decoder,
                    aws_event_stream_message_prelude* prelude,
                    aws_event_stream_header_value_pair* header,
                    void* context);

                /**
                 * Callback function invoked when an error is encountered.
                 * @param decoder The underlying decoder defined in aws-c-event-stream.
                 * @param prelude The metadata of the message, including total message length and header length.
                 * @param errorCode Error code indicates the type of the error encountered.
                 * @param message Error message indicates the details of the error encountered.
                 * @param context A context pointer, will cast it to a pointer of flow handler.
                 */
                static void onError(
                    aws_event_stream_streaming_decoder* decoder,
                    aws_event_stream_message_prelude* prelude,
                    int errorCode,
                    const char* message,
                    void* context);

                /**
                 * The underlying decoder defined in aws-c-event-stream.
                 * The decoder will invoke callback functions when the streaming messages received.
                 */
                aws_event_stream_streaming_decoder m_decoder;
                EventStreamHandler* m_eventStreamHandler;
            };
        }
    }
}
