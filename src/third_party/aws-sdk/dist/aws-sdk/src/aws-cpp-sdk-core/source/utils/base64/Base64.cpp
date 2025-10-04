/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/base64/Base64.h>
#include <cstring>

using namespace Aws::Utils::Base64;

static const uint8_t SENTINEL_VALUE = 255;
static const char BASE64_ENCODING_TABLE_MIME[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

namespace Aws
{
namespace Utils
{
namespace Base64
{

Base64::Base64(const char *encodingTable)
{
    if(encodingTable == nullptr)
    {
        encodingTable = BASE64_ENCODING_TABLE_MIME;
    }

    size_t encodingTableLength = strlen(encodingTable);
    if(encodingTableLength != 64)
    {
        encodingTable = BASE64_ENCODING_TABLE_MIME;
        encodingTableLength = 64;
    }

    memcpy(m_mimeBase64EncodingTable, encodingTable, encodingTableLength);

    memset((void *)m_mimeBase64DecodingTable, 0, 256);

    for(uint32_t i = 0; i < encodingTableLength; ++i)
    {
        uint32_t index = static_cast<uint32_t>(m_mimeBase64EncodingTable[i]);
        m_mimeBase64DecodingTable[index] = static_cast<uint8_t>(i);
    }

    m_mimeBase64DecodingTable[(uint32_t)'='] = SENTINEL_VALUE;
}

Aws::String Base64::Encode(const Aws::Utils::ByteBuffer& buffer) const
{
    size_t bufferLength = buffer.GetLength();
    size_t blockCount = (bufferLength + 2) / 3;
    size_t remainderCount = (bufferLength % 3);

    Aws::String outputString;
    outputString.reserve(CalculateBase64EncodedLength(buffer));

    for(size_t i = 0; i < bufferLength; i += 3 )
    {
        uint32_t block = buffer[ i ];

        block <<= 8;
        if (i + 1 < bufferLength)
        {
            block = block | buffer[ i + 1 ];
        }

        block <<= 8;
        if (i + 2 < bufferLength)
        {
            block = block | buffer[ i + 2 ];
        }

        outputString.push_back(m_mimeBase64EncodingTable[(block >> 18) & 0x3F]);
        outputString.push_back(m_mimeBase64EncodingTable[(block >> 12) & 0x3F]);
        outputString.push_back(m_mimeBase64EncodingTable[(block >> 6) & 0x3F]);
        outputString.push_back(m_mimeBase64EncodingTable[block & 0x3F]);
    }

    if(remainderCount > 0)
    {
        outputString[blockCount * 4 - 1] = '=';
        if(remainderCount == 1)
        {
            outputString[blockCount * 4 - 2] = '=';
        }
    }

    return outputString;
}

Aws::Utils::ByteBuffer Base64::Decode(const Aws::String& str) const
{
    size_t decodedLength = CalculateBase64DecodedLength(str);

    Aws::Utils::ByteBuffer buffer(decodedLength);

    const char* rawString = str.c_str();
    size_t blockCount = str.length() / 4;
    for(size_t i = 0; i < blockCount; ++i)
    {
        size_t stringIndex = i * 4;

        uint32_t value1 = m_mimeBase64DecodingTable[uint32_t(rawString[stringIndex])];
        uint32_t value2 = m_mimeBase64DecodingTable[uint32_t(rawString[++stringIndex])];
        uint32_t value3 = m_mimeBase64DecodingTable[uint32_t(rawString[++stringIndex])];
        uint32_t value4 = m_mimeBase64DecodingTable[uint32_t(rawString[++stringIndex])];

        size_t bufferIndex = i * 3;
        buffer[bufferIndex] = static_cast<uint8_t>((value1 << 2) | ((value2 >> 4) & 0x03));
        if(value3 != SENTINEL_VALUE)
        {
            buffer[++bufferIndex] = static_cast<uint8_t>(((value2 << 4) & 0xF0) | ((value3 >> 2) & 0x0F));
            if(value4 != SENTINEL_VALUE)
            {
                buffer[++bufferIndex] = static_cast<uint8_t>((value3 & 0x03) << 6 | value4);
            }
        }
    }

    return buffer;
}

size_t Base64::CalculateBase64DecodedLength(const Aws::String& b64input)
{
    const size_t len = b64input.length();
    if(len < 2)
    {
        return 0;
    }

    size_t padding = 0;

    if (b64input[len - 1] == '=' && b64input[len - 2] == '=') //last two chars are =
        padding = 2;
    else if (b64input[len - 1] == '=') //last char is =
        padding = 1;

    return (len * 3 / 4 - padding);
}

size_t Base64::CalculateBase64EncodedLength(const Aws::Utils::ByteBuffer& buffer)
{
    return 4 * ((buffer.GetLength() + 2) / 3);
}

} // namespace Base64
} // namespace Utils
} // namespace Aws