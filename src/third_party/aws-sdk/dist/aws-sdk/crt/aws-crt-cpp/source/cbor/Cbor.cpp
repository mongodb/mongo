/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Api.h>
#include <aws/crt/cbor/Cbor.h>

namespace Aws
{
    namespace Crt
    {
        namespace Cbor
        {
            /*****************************************************
             *
             * CborEncoder
             *
             *****************************************************/
            CborEncoder::CborEncoder(Crt::Allocator *allocator) noexcept
            {
                m_encoder = aws_cbor_encoder_new(allocator);
            }

            CborEncoder::~CborEncoder() noexcept
            {
                aws_cbor_encoder_destroy(m_encoder);
            }

            ByteCursor CborEncoder::GetEncodedData() noexcept
            {
                return aws_cbor_encoder_get_encoded_data(m_encoder);
            }
            void CborEncoder::Reset() noexcept
            {
                aws_cbor_encoder_reset(m_encoder);
            }

            void CborEncoder::WriteUInt(uint64_t value) noexcept
            {
                aws_cbor_encoder_write_uint(m_encoder, value);
            }
            void CborEncoder::WriteNegInt(uint64_t value) noexcept
            {
                aws_cbor_encoder_write_negint(m_encoder, value);
            }

            void CborEncoder::WriteFloat(double value) noexcept
            {
                aws_cbor_encoder_write_float(m_encoder, value);
            }

            void CborEncoder::WriteBytes(ByteCursor value) noexcept
            {
                aws_cbor_encoder_write_bytes(m_encoder, value);
            }

            void CborEncoder::WriteText(ByteCursor value) noexcept
            {
                aws_cbor_encoder_write_text(m_encoder, value);
            }

            void CborEncoder::WriteArrayStart(size_t number_entries) noexcept
            {
                aws_cbor_encoder_write_array_start(m_encoder, number_entries);
            }

            void CborEncoder::WriteMapStart(size_t number_entries) noexcept
            {
                aws_cbor_encoder_write_map_start(m_encoder, number_entries);
            }

            void CborEncoder::WriteTag(uint64_t tag_number) noexcept
            {
                aws_cbor_encoder_write_tag(m_encoder, tag_number);
            }

            void CborEncoder::WriteNull() noexcept
            {
                aws_cbor_encoder_write_null(m_encoder);
            }

            void CborEncoder::WriteUndefined() noexcept
            {
                aws_cbor_encoder_write_undefined(m_encoder);
            }

            void CborEncoder::WriteBool(bool value) noexcept
            {
                aws_cbor_encoder_write_bool(m_encoder, value);
            }

            void CborEncoder::WriteBreak() noexcept
            {
                aws_cbor_encoder_write_break(m_encoder);
            }

            void CborEncoder::WriteIndefBytesStart() noexcept
            {
                aws_cbor_encoder_write_indef_bytes_start(m_encoder);
            }

            void CborEncoder::WriteIndefTextStart() noexcept
            {
                aws_cbor_encoder_write_indef_text_start(m_encoder);
            }

            void CborEncoder::WriteIndefArrayStart() noexcept
            {
                aws_cbor_encoder_write_indef_array_start(m_encoder);
            }

            void CborEncoder::WriteIndefMapStart() noexcept
            {
                aws_cbor_encoder_write_indef_map_start(m_encoder);
            }

            /*****************************************************
             *
             * CborDecoder
             *
             *****************************************************/
            CborDecoder::CborDecoder(ByteCursor src, Crt::Allocator *allocator) noexcept
            {
                m_decoder = aws_cbor_decoder_new(allocator, src);
            }

            CborDecoder::~CborDecoder() noexcept
            {
                aws_cbor_decoder_destroy(m_decoder);
            }

            size_t CborDecoder::GetRemainingLength() noexcept
            {
                return aws_cbor_decoder_get_remaining_length(m_decoder);
            }

            Optional<CborType> CborDecoder::PeekType() noexcept
            {
                enum aws_cbor_type out_type_c = AWS_CBOR_TYPE_UNKNOWN;
                if (aws_cbor_decoder_peek_type(m_decoder, &out_type_c) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<CborType>();
                }
                return Optional<CborType>((CborType)out_type_c);
            }
            bool CborDecoder::ConsumeNextWholeDataItem() noexcept
            {
                if (aws_cbor_decoder_consume_next_whole_data_item(m_decoder) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return false;
                }
                return true;
            }

            bool CborDecoder::ConsumeNextSingleElement() noexcept
            {
                if (aws_cbor_decoder_consume_next_single_element(m_decoder) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return false;
                }
                return true;
            }

            Optional<uint64_t> CborDecoder::PopNextUnsignedIntVal() noexcept
            {
                uint64_t out = 0;
                if (aws_cbor_decoder_pop_next_unsigned_int_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<uint64_t>();
                }
                return Optional<uint64_t>(out);
            }

            Optional<uint64_t> CborDecoder::PopNextNegativeIntVal() noexcept
            {
                uint64_t out = 0;
                if (aws_cbor_decoder_pop_next_negative_int_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<uint64_t>();
                }
                return Optional<uint64_t>(out);
            }

            Optional<double> CborDecoder::PopNextFloatVal() noexcept
            {
                double out = 0;
                if (aws_cbor_decoder_pop_next_float_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<double>();
                }
                return Optional<double>(out);
            }

            Optional<bool> CborDecoder::PopNextBooleanVal() noexcept
            {
                bool out = false;
                if (aws_cbor_decoder_pop_next_boolean_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<bool>();
                }
                return Optional<bool>(out);
            }

            Optional<ByteCursor> CborDecoder::PopNextBytesVal() noexcept
            {
                ByteCursor out = {0};
                if (aws_cbor_decoder_pop_next_bytes_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<ByteCursor>();
                }
                return Optional<ByteCursor>(out);
            }

            Optional<ByteCursor> CborDecoder::PopNextTextVal() noexcept
            {
                ByteCursor out = {0};
                if (aws_cbor_decoder_pop_next_text_val(m_decoder, &out) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<ByteCursor>();
                }
                return Optional<ByteCursor>(out);
            }

            Optional<uint64_t> CborDecoder::PopNextArrayStart() noexcept
            {
                uint64_t out_size = 0;
                if (aws_cbor_decoder_pop_next_array_start(m_decoder, &out_size) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<uint64_t>();
                }
                return Optional<uint64_t>(out_size);
            }

            Optional<uint64_t> CborDecoder::PopNextMapStart() noexcept
            {
                uint64_t out_size = 0;
                if (aws_cbor_decoder_pop_next_map_start(m_decoder, &out_size) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<uint64_t>();
                }
                return Optional<uint64_t>(out_size);
            }

            Optional<uint64_t> CborDecoder::PopNextTagVal() noexcept
            {
                uint64_t out_tag_val = 0;
                if (aws_cbor_decoder_pop_next_tag_val(m_decoder, &out_tag_val) != AWS_OP_SUCCESS)
                {
                    m_lastError = aws_last_error();
                    return Optional<uint64_t>();
                }
                return Optional<uint64_t>(out_tag_val);
            }

        } // namespace Cbor
    } // namespace Crt
} // namespace Aws
