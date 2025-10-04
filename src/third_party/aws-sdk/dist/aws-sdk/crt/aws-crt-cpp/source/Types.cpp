/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Types.h>

#include <aws/common/encoding.h>

namespace Aws
{
    namespace Crt
    {
        ByteBuf ByteBufFromCString(const char *str) noexcept
        {
            return aws_byte_buf_from_c_str(str);
        }

        ByteBuf ByteBufFromEmptyArray(const uint8_t *array, size_t len) noexcept
        {
            return aws_byte_buf_from_empty_array(array, len);
        }

        ByteBuf ByteBufFromArray(const uint8_t *array, size_t capacity) noexcept
        {
            return aws_byte_buf_from_array(array, capacity);
        }

        ByteBuf ByteBufNewCopy(Allocator *alloc, const uint8_t *array, size_t len)
        {
            ByteBuf retVal;
            ByteBuf src = aws_byte_buf_from_array(array, len);
            aws_byte_buf_init_copy(&retVal, alloc, &src);
            return retVal;
        }

        ByteBuf ByteBufInit(Allocator *alloc, size_t len)
        {
            ByteBuf buff;
            aws_byte_buf_init(&buff, alloc, len);
            return buff;
        }

        void ByteBufDelete(ByteBuf &buf)
        {
            aws_byte_buf_clean_up(&buf);
        }

        ByteCursor ByteCursorFromCString(const char *str) noexcept
        {
            return aws_byte_cursor_from_c_str(str);
        }

        ByteCursor ByteCursorFromString(const Crt::String &str) noexcept
        {
            return aws_byte_cursor_from_array((const void *)str.data(), str.length());
        }

        ByteCursor ByteCursorFromStringView(const Crt::StringView &str) noexcept
        {
            return aws_byte_cursor_from_array((const void *)str.data(), str.length());
        }

        ByteCursor ByteCursorFromByteBuf(const ByteBuf &buf) noexcept
        {
            return aws_byte_cursor_from_buf(&buf);
        }

        ByteCursor ByteCursorFromArray(const uint8_t *array, size_t len) noexcept
        {
            return aws_byte_cursor_from_array(array, len);
        }

        Vector<uint8_t> Base64Decode(const String &decode) noexcept
        {
            ByteCursor toDecode = ByteCursorFromString(decode);

            size_t allocationSize = 0;

            if (AWS_OP_SUCCESS != aws_base64_compute_decoded_len(&toDecode, &allocationSize))
            {
                return {};
            }

            Vector<uint8_t> output(allocationSize, 0x00);
            ByteBuf tempBuf = aws_byte_buf_from_empty_array(output.data(), output.size());

            if (AWS_OP_SUCCESS != aws_base64_decode(&toDecode, &tempBuf))
            {
                return {};
            }

            return output;
        }

        String Base64Encode(const Vector<uint8_t> &encode) noexcept
        {
            auto toEncode = aws_byte_cursor_from_array((const void *)encode.data(), encode.size());

            size_t allocationSize = 0;
            if (AWS_OP_SUCCESS != aws_base64_compute_encoded_len(toEncode.len, &allocationSize))
            {
                return {};
            }

            String outputStr(allocationSize, 0x00);
            auto tempBuf = aws_byte_buf_from_empty_array(outputStr.data(), outputStr.size());

            if (AWS_OP_SUCCESS != aws_base64_encode(&toEncode, &tempBuf))
            {
                return {};
            }

            // encoding appends a null terminator, and accounts for it in the encoded length,
            // which makes the string 1 character too long
            if (outputStr.back() == 0)
            {
                outputStr.pop_back();
            }
            return outputStr;
        }
    } // namespace Crt
} // namespace Aws
