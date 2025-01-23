#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/cbor.h>

#include <aws/crt/Types.h>

namespace Aws
{
    namespace Crt
    {
        namespace Cbor
        {
            /**
             * The types used by APIs, not 1:1 with major types.
             * It's an extension for CBOR major type in RFC8949 section 3.1.
             * Major type 0 - UInt
             * Major type 1 - NegInt
             * Major type 2 - Bytes / IndefBytesStart
             * Major type 3 - Text / IndefTextStart
             * Major type 4 - ArrayStart / IndefArrayStart
             * Major type 5 - MapStart / IndefMapStart
             * Major type 6 - Tag
             * Major type 7:
             *  - 20/21 - Bool
             *  - 22 - Null
             *  - 23 - Undefined
             *  - 25/26/27 - Float
             *  - 31 - Break
             *  - Rest of the values are not supported.
             */
            enum class CborType
            {
                Unknown = AWS_CBOR_TYPE_UNKNOWN,
                UInt = AWS_CBOR_TYPE_UINT,
                NegInt = AWS_CBOR_TYPE_NEGINT,
                Float = AWS_CBOR_TYPE_FLOAT,
                Bytes = AWS_CBOR_TYPE_BYTES,
                Text = AWS_CBOR_TYPE_TEXT,
                ArrayStart = AWS_CBOR_TYPE_ARRAY_START,
                MapStart = AWS_CBOR_TYPE_MAP_START,
                Tag = AWS_CBOR_TYPE_TAG,
                Bool = AWS_CBOR_TYPE_BOOL,
                Null = AWS_CBOR_TYPE_NULL,
                Undefined = AWS_CBOR_TYPE_UNDEFINED,
                Break = AWS_CBOR_TYPE_BREAK,
                IndefBytesStart = AWS_CBOR_TYPE_INDEF_BYTES_START,
                IndefTextStart = AWS_CBOR_TYPE_INDEF_TEXT_START,
                IndefArrayStart = AWS_CBOR_TYPE_INDEF_ARRAY_START,
                IndefMapStart = AWS_CBOR_TYPE_INDEF_MAP_START,
            };

            class AWS_CRT_CPP_API CborEncoder final
            {
              public:
                CborEncoder(const CborEncoder &) = delete;
                CborEncoder(CborEncoder &&) = delete;
                CborEncoder &operator=(const CborEncoder &) = delete;
                CborEncoder &operator=(CborEncoder &&) = delete;

                CborEncoder(Allocator *allocator = ApiAllocator()) noexcept;
                ~CborEncoder() noexcept;

                /**
                 * Get the current encoded data from encoder. The encoded data has the same lifetime as the encoder,
                 * and once any other function call invoked for the encoder, the encoded data is no longer valid.
                 *
                 * @return the current encoded data
                 */
                ByteCursor GetEncodedData() noexcept;

                /**
                 * Clear the current encoded buffer from encoder.
                 */
                void Reset() noexcept;

                /**
                 * Encode a AWS_CBOR_TYPE_UINT value to "smallest possible" in encoder's buffer.
                 *  Referring to RFC8949 section 4.2.1
                 *
                 * @param value value to encode.
                 */
                void WriteUInt(uint64_t value) noexcept;

                /**
                 * Encode a AWS_CBOR_TYPE_NEGINT value to "smallest possible" in encoder's buffer.
                 * It represents (-1 - value).
                 * Referring to RFC8949 section 4.2.1
                 *
                 * @param value value to encode, which is (-1 - represented value)
                 */
                void WriteNegInt(uint64_t value) noexcept;

                /**
                 * Encode a AWS_CBOR_TYPE_FLOAT value to "smallest possible", but will not be encoded into
                 * half-precision float, as it's not well supported cross languages.
                 *
                 * To be more specific, it will be encoded into integer/negative/float
                 * (Order with priority) when the conversion will not cause precision loss.
                 *
                 * @param value value to encode.
                 */
                void WriteFloat(double value) noexcept;

                /**
                 * Encode a Bytes value to "smallest possible" in encoder's buffer.
                 * Referring to RFC8949 section 4.2.1, the length of "value" will be encoded first and then the value of
                 * "value" will be followed.
                 *
                 * @param value value to encode.
                 */
                void WriteBytes(ByteCursor value) noexcept;

                /**
                 * Encode a Text value to "smallest possible" in encoder's buffer.
                 * Referring to RFC8949 section 4.2.1, the length of "value" will be encoded first and then the value of
                 * "value" will be followed.
                 *
                 * @param value value to encode.
                 */
                void WriteText(ByteCursor value) noexcept;

                /**
                 * Encode an ArrayStart value to "smallest possible" in encoder's buffer.
                 * Referring to RFC8949 section 4.2.1
                 * Notes: it's user's responsibility to keep the integrity of the array to be encoded.
                 *
                 * @param number_entries the number of CBOR data items to be followed as the content of the array.
                 */
                void WriteArrayStart(size_t number_entries) noexcept;

                /**
                 * Encode a MapStart value to "smallest possible" in encoder's buffer.
                 * Referring to RFC8949 section 4.2.1
                 *
                 * Notes: it's user's responsibility to keep the integrity of the map to be encoded.
                 *
                 * @param number_entries the number of pair of CBOR data items as key and value to be followed as
                 * the content of the map.
                 */
                void WriteMapStart(size_t number_entries) noexcept;

                /**
                 * Encode a Tag value to "smallest possible" in encoder's buffer.
                 * Referring to RFC8949 section 4.2.1
                 * The following CBOR data item will be the content of the tagged value.
                 * Notes: it's user's responsibility to keep the integrity of the tagged value to follow the RFC8949
                 * section 3.4
                 *
                 * @param tag_number The tag value to encode.
                 */
                void WriteTag(uint64_t tag_number) noexcept;

                /**
                 * Encode a simple value Null
                 */
                void WriteNull() noexcept;

                /**
                 * Encode a simple value Undefined
                 */
                void WriteUndefined() noexcept;

                /**
                 * Encode a simple value Bool
                 */
                void WriteBool(bool value) noexcept;

                /**
                 * Encode a simple value Break
                 * Notes: no error checking, it's user's responsibility to track the break to close the corresponding
                 * indef_start
                 */
                void WriteBreak() noexcept;

                /**
                 * Encode an IndefBytesStart
                 * Notes: no error checking, it's user's responsibility to add corresponding data and the break to close
                 * the indef_start
                 */
                void WriteIndefBytesStart() noexcept;

                /**
                 * Encode an IndefTextStart
                 * Notes: no error checking, it's user's responsibility to add corresponding data and the break to close
                 * the indef_start
                 */
                void WriteIndefTextStart() noexcept;

                /**
                 * Encode an IndefArrayStart
                 * Notes: no error checking, it's user's responsibility to add corresponding data and the break to close
                 * the indef_start
                 */
                void WriteIndefArrayStart() noexcept;

                /**
                 * Encode an IndefMapStart
                 * Notes: no error checking, it's user's responsibility to add corresponding data and the break to close
                 * the indef_start
                 */
                void WriteIndefMapStart() noexcept;

              private:
                struct aws_cbor_encoder *m_encoder;
            };

            class AWS_CRT_CPP_API CborDecoder final
            {

              public:
                CborDecoder(const CborDecoder &) = delete;
                CborDecoder(CborDecoder &&) = delete;
                CborDecoder &operator=(const CborDecoder &) = delete;
                CborDecoder &operator=(CborDecoder &&) = delete;

                /**
                 * Construct a new Cbor Decoder object
                 *
                 * @param allocator
                 * @param src The src data to decode from.
                 */
                CborDecoder(ByteCursor src, Allocator *allocator = ApiAllocator()) noexcept;
                ~CborDecoder() noexcept;

                /**
                 * Get the length of the remaining bytes of the source. Once the source was decoded, it will be
                 * consumed, and result in decrease of the remaining length of bytes.
                 *
                 * @return The length of bytes remaining of the decoder source.
                 */
                size_t GetRemainingLength() noexcept;

                /**
                 * Decode the next element and store it in the decoder cache if there was no element cached.
                 * If there was an element cached, just return the type of the cached element.
                 *
                 * @return If successful, return the type of next element
                 *         If not, return will be none and LastError() can be
                 *          used to retrieve CRT error code.
                 */
                Optional<CborType> PeekType() noexcept;

                /**
                 * Consume the next data item, includes all the content within the data item.
                 *
                 * As an example for the following CBOR, this function will consume all the data
                 * as it's only one CBOR data item, an indefinite map with 2 key, value pair:
                 * 0xbf6346756ef563416d7421ff
                 * BF          -- Start indefinite-length map
                 *   63        -- First key, UTF-8 string length 3
                 *      46756e --   "Fun"
                 *   F5        -- First value, true
                 *   63        -- Second key, UTF-8 string length 3
                 *      416d74 --   "Amt"
                 *   21        -- Second value, -2
                 *   FF        -- "break"
                 *
                 * Notes: this function will not ensure the data item is well-formed.
                 *
                 * @return true if the operation succeed, false otherwise and LastError() will contain the errorCode.
                 */
                bool ConsumeNextWholeDataItem() noexcept;

                /**
                 * Consume the next single element, without the content followed by the element.
                 *
                 * As an example for the following CBOR, this function will only consume the
                 * 0xBF, "Start indefinite-length map", not any content of the map represented.
                 * The next element to decode will start from 0x63.
                 * 0xbf6346756ef563416d7421ff
                 * BF          -- Start indefinite-length map
                 *   63        -- First key, UTF-8 string length 3
                 *      46756e --   "Fun"
                 *   F5        -- First value, true
                 *   63        -- Second key, UTF-8 string length 3
                 *      416d74 --   "Amt"
                 *   21        -- Second value, -2
                 *   FF        -- "break"
                 *
                 * @return true if the operation succeed, false otherwise and LastError() will contain the errorCode.
                 */
                bool ConsumeNextSingleElement() noexcept;

                /**
                 * Get the next element based on the type. If the next element doesn't match the expected type, an error
                 * will be raised. If the next element has already been cached, it will consume the cached item when no
                 * error was returned. Specifically:
                 *  - UInt - PopNextUnsignedIntVal
                 *  - NegInt - PopNextNegativeIntVal, it represents (-1 - &out)
                 *  - Float - PopNextFloatVal
                 *  - Bytes - PopNextBytesVal
                 *  - Text - PopNextTextVal
                 *
                 * @return If successful, return the next element
                 *         If not, return will be none and LastError() can be
                 *          used to retrieve CRT error code.
                 */
                Optional<uint64_t> PopNextUnsignedIntVal() noexcept;
                Optional<uint64_t> PopNextNegativeIntVal() noexcept;
                Optional<double> PopNextFloatVal() noexcept;
                Optional<bool> PopNextBooleanVal() noexcept;
                Optional<ByteCursor> PopNextBytesVal() noexcept;
                Optional<ByteCursor> PopNextTextVal() noexcept;

                /**
                 * Get the next ArrayStart element. Only consume the ArrayStart element and set the size of array to
                 * &out_size, not the content of the array. The next &out_size CBOR data items will be the content of
                 * the array for a valid CBOR data.
                 *
                 * Notes: For indefinite-length, this function will fail with "AWS_ERROR_CBOR_UNEXPECTED_TYPE". The
                 * designed way to handle indefinite-length is:
                 * - Get IndefArrayStart from PeekType
                 * - Call ConsumeNextSingleElement to pop the indefinite-length start.
                 * - Decode the next data item until Break is read.
                 *
                 * @return If successful, return the size of array
                 *         If not, return will be none and LastError() can be
                 *          used to retrieve CRT error code.
                 */
                Optional<uint64_t> PopNextArrayStart() noexcept;

                /**
                 * Get the next MapStart element. Only consume the MapStart element and set the size of array to
                 * &out_size, not the content of the map. The next &out_size pair of CBOR data items as key and value
                 * will be the content of the array for a valid CBOR data.
                 *
                 * Notes: For indefinite-length, this function will fail with "AWS_ERROR_CBOR_UNEXPECTED_TYPE". The
                 * designed way to handle indefinite-length is:
                 * - Get IndefMapStart from PeekType
                 * - Call ConsumeNextSingleElement to pop the indefinite-length start.
                 * - Decode the next data item until Break is read.
                 *
                 * @return If successful, return the size of map
                 *         If not, return will be none and LastError() can be
                 *          used to retrieve CRT error code.
                 */
                Optional<uint64_t> PopNextMapStart() noexcept;

                /**
                 * Get the next Tag element. Only consume the Tag element and set the tag value to out_tag_val,
                 * not the content of the tagged value. The next CBOR data item will be the content of the tagged value
                 * for a valid CBOR data.
                 *
                 * @return If successful, return the tag value
                 *         If not, return will be none and LastError() can be
                 *          used to retrieve CRT error code.
                 */
                Optional<uint64_t> PopNextTagVal() noexcept;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const noexcept { return m_lastError ? m_lastError : AWS_ERROR_UNKNOWN; }

              private:
                struct aws_cbor_decoder *m_decoder;
                /* Error */
                int m_lastError;
            };
        } // namespace Cbor

    } // namespace Crt
} // namespace Aws
