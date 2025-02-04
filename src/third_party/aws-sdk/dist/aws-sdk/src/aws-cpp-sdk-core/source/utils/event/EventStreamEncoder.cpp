/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/event/EventHeader.h>
#include <aws/core/utils/event/EventMessage.h>
#include <aws/core/utils/event/EventStreamEncoder.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/common/byte_order.h>
#include <aws/core/utils/memory/AWSMemory.h>

#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            static const char TAG[] = "EventStreamEncoder";

            static void EncodeHeaders(const Aws::Utils::Event::Message& msg, aws_array_list* headers)
            {
                aws_array_list_init_dynamic(headers, get_aws_allocator(), msg.GetEventHeaders().size(), sizeof(aws_event_stream_header_value_pair));
                for (auto&& header : msg.GetEventHeaders())
                {
                    const uint8_t headerKeyLen = static_cast<uint8_t>(header.first.length());
                    switch(header.second.GetType())
                    {
                        case EventHeaderValue::EventHeaderType::BOOL_TRUE:
                        case EventHeaderValue::EventHeaderType::BOOL_FALSE:
                            aws_event_stream_add_bool_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsBoolean());
                            break;
                        case EventHeaderValue::EventHeaderType::BYTE:
                            aws_event_stream_add_bool_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsByte());
                            break;
                        case EventHeaderValue::EventHeaderType::INT16:
                            aws_event_stream_add_int16_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsInt16());
                            break;
                        case EventHeaderValue::EventHeaderType::INT32:
                            aws_event_stream_add_int32_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsInt32());
                            break;
                        case EventHeaderValue::EventHeaderType::INT64:
                            aws_event_stream_add_int64_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsInt64());
                            break;
                        case EventHeaderValue::EventHeaderType::BYTE_BUF:
                            {
                                const auto& bytes = header.second.GetEventHeaderValueAsBytebuf();
                                aws_event_stream_add_bytebuf_header(headers, header.first.c_str(), headerKeyLen, bytes.GetUnderlyingData(), static_cast<uint16_t>(bytes.GetLength()), 1 /*copy*/);
                            }
                            break;
                        case EventHeaderValue::EventHeaderType::STRING:
                            {
                                const auto& bytes = header.second.GetUnderlyingBuffer();
                                aws_event_stream_add_string_header(headers, header.first.c_str(), headerKeyLen, reinterpret_cast<char*>(bytes.GetUnderlyingData()), static_cast<uint16_t>(bytes.GetLength()), 0 /*copy*/);
                            }
                            break;
                        case EventHeaderValue::EventHeaderType::TIMESTAMP:
                            aws_event_stream_add_timestamp_header(headers, header.first.c_str(), headerKeyLen, header.second.GetEventHeaderValueAsTimestamp());
                            break;
                        case EventHeaderValue::EventHeaderType::UUID:
                            {
                                ByteBuffer uuidBytes = header.second.GetEventHeaderValueAsUuid();
                                aws_event_stream_add_uuid_header(headers, header.first.c_str(), headerKeyLen, uuidBytes.GetUnderlyingData());
                            }
                            break;
                        default:
                            AWS_LOG_ERROR(TAG, "Encountered unknown type of header.");
                            break;
                    }
                }
            }

            EventStreamEncoder::EventStreamEncoder(Client::AWSAuthSigner* signer) : m_signer(signer)
            {
            }


            Aws::Vector<unsigned char> EventStreamEncoder::EncodeAndSign(const Aws::Utils::Event::Message& msg)
            {
                Aws::Vector<unsigned char> outputBits;

                aws_event_stream_message encoded;
                aws_event_stream_message* encodedPayload = nullptr;
                bool msgEncodeSuccess = true; // empty message "successes" to encode
                if (!msg.GetEventHeaders().empty() || !msg.GetEventPayload().empty())
                {
                    InitEncodedStruct(msg, &encoded);
                    encodedPayload = &encoded;
                }

                if (msgEncodeSuccess)
                {
                    aws_event_stream_message signedMessage;
                    if (InitSignedStruct(encodedPayload, &signedMessage))
                    {
                        // success!
                        const auto signedMessageBuffer = aws_event_stream_message_buffer(&signedMessage);
                        const auto signedMessageLength = aws_event_stream_message_total_length(&signedMessage);
                        outputBits.reserve(signedMessageLength);
                        outputBits.insert(outputBits.end(), signedMessageBuffer, signedMessageBuffer + signedMessageLength);

                        aws_event_stream_message_clean_up(&signedMessage);
                    }
                    if (encodedPayload)
                    {
                        aws_event_stream_message_clean_up(encodedPayload);
                    }
                }

                return outputBits;
            }

            bool EventStreamEncoder::InitEncodedStruct(const Aws::Utils::Event::Message& msg, aws_event_stream_message* encoded)
            {
                bool success = false;

                aws_array_list headers;
                EncodeHeaders(msg, &headers);

                aws_byte_buf payload = aws_byte_buf_from_array(msg.GetEventPayload().data(), msg.GetEventPayload().size());

                if(aws_event_stream_message_init(encoded, get_aws_allocator(), &headers, &payload) == AWS_OP_SUCCESS)
                {
                    success = true;
                }
                else
                {
                    AWS_LOGSTREAM_ERROR(TAG, "Error creating event-stream message from payload.");
                }

                aws_event_stream_headers_list_cleanup(&headers);
                return success;
            }

            bool EventStreamEncoder::InitSignedStruct(const aws_event_stream_message* payload, aws_event_stream_message* signedmsg)
            {
                bool success = false;

                Event::Message signedMessage;
                if (payload)
                {
                    const auto msgbuf = aws_event_stream_message_buffer(payload);
                    const auto msglen = aws_event_stream_message_total_length(payload);
                    signedMessage.WriteEventPayload(msgbuf, msglen);
                }

                assert(m_signer);
                if (m_signer->SignEventMessage(signedMessage, m_signatureSeed))
                {
                    aws_array_list headers;
                    EncodeHeaders(signedMessage, &headers);

                    aws_byte_buf signedPayload = aws_byte_buf_from_array(signedMessage.GetEventPayload().data(), signedMessage.GetEventPayload().size());

                    if(aws_event_stream_message_init(signedmsg, get_aws_allocator(), &headers, &signedPayload) == AWS_OP_SUCCESS)
                    {
                        success = true;
                    }
                    else
                    {
                        AWS_LOGSTREAM_ERROR(TAG, "Error creating event-stream message from payload.");
                    }
                    aws_event_stream_headers_list_cleanup(&headers);
                }
                else
                {
                    AWS_LOGSTREAM_ERROR(TAG, "Failed to sign event message frame.");
                }

                return success;
            }

        } // namespace Event
    } // namespace Utils
} // namespace Aws

