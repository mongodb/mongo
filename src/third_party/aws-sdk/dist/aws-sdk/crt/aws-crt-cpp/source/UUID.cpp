/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/UUID.h>

namespace Aws
{
    namespace Crt
    {
        UUID::UUID() noexcept : m_good(false)
        {
            if (aws_uuid_init(&m_uuid) == AWS_OP_SUCCESS)
            {
                m_good = true;
            }
        }

        UUID::UUID(const String &str) noexcept : m_good(false)
        {
            auto strCur = aws_byte_cursor_from_c_str(str.c_str());
            if (aws_uuid_init_from_str(&m_uuid, &strCur) == AWS_OP_SUCCESS)
            {
                m_good = true;
            }
        }

        UUID &UUID::operator=(const String &str) noexcept
        {
            *this = UUID(str);
            return *this;
        }

        bool UUID::operator==(const UUID &other) noexcept
        {
            return aws_uuid_equals(&m_uuid, &other.m_uuid);
        }

        bool UUID::operator!=(const UUID &other) noexcept
        {
            return !aws_uuid_equals(&m_uuid, &other.m_uuid);
        }

        String UUID::ToString() const
        {
            String uuidStr;
            uuidStr.resize(AWS_UUID_STR_LEN);
            auto outBuf = ByteBufFromEmptyArray(reinterpret_cast<const uint8_t *>(uuidStr.data()), uuidStr.capacity());
            aws_uuid_to_str(&m_uuid, &outBuf);
            uuidStr.resize(outBuf.len);
            return uuidStr;
        }

        UUID::operator String() const
        {
            return ToString();
        }

        UUID::operator ByteBuf() const noexcept
        {
            return ByteBufFromArray(m_uuid.uuid_data, sizeof(m_uuid.uuid_data));
        }

        int UUID::GetLastError() const noexcept
        {
            return aws_last_error();
        }
    } // namespace Crt
} // namespace Aws