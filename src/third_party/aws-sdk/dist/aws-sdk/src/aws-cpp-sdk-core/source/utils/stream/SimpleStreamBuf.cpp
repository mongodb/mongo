
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/stream/SimpleStreamBuf.h>
#include <aws/core/utils/logging/LogMacros.h>

#include <algorithm>
#include <cassert>
#include <cstring>

namespace Aws
{
namespace Utils
{
namespace Stream
{

static const uint32_t DEFAULT_BUFFER_SIZE = 100;
static const char* SIMPLE_STREAMBUF_ALLOCATION_TAG = "SimpleStreamBufTag";

SimpleStreamBuf::SimpleStreamBuf() :
    m_buffer(nullptr),
    m_bufferSize(0)
{
    m_buffer = Aws::NewArray<char>(DEFAULT_BUFFER_SIZE, SIMPLE_STREAMBUF_ALLOCATION_TAG);
    m_bufferSize = DEFAULT_BUFFER_SIZE;

    char* begin = m_buffer;
    char* end = begin + m_bufferSize;

    setp(begin, end);
    setg(begin, begin, begin);
}

SimpleStreamBuf::SimpleStreamBuf(const Aws::String& value) :
    m_buffer(nullptr),
    m_bufferSize(0)
{
    size_t baseSize = (std::max)(value.size(), static_cast<std::size_t>(DEFAULT_BUFFER_SIZE));

    m_buffer = Aws::NewArray<char>(baseSize, SIMPLE_STREAMBUF_ALLOCATION_TAG);
    m_bufferSize = baseSize;

    std::memcpy(m_buffer, value.c_str(), value.size());

    char* begin = m_buffer;
    char* end = begin + m_bufferSize;

    setp(begin + value.size(), end);
    setg(begin, begin, begin);
}

SimpleStreamBuf::~SimpleStreamBuf()
{
    if(m_buffer)
    {
        Aws::DeleteArray<char>(m_buffer);
        m_buffer = nullptr;
    }

    m_bufferSize = 0;
}

std::streampos SimpleStreamBuf::seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode which)
{
    if (dir == std::ios_base::beg)
    {
        return seekpos(off, which);
    }
    else if (dir == std::ios_base::end)
    {
        return seekpos((pptr() - m_buffer) - off, which);
    }
    else if (dir == std::ios_base::cur)
    {
        if(which == std::ios_base::in)
        { 
            return seekpos((gptr() - m_buffer) + off, which);
        }
        else
        {
            return seekpos((pptr() - m_buffer) + off, which);
        }
    }

    return off_type(-1);
}

std::streampos SimpleStreamBuf::seekpos(std::streampos pos, std::ios_base::openmode which)
{
    size_t maxSeek = pptr() - m_buffer;
    assert(static_cast<size_t>(pos) <= maxSeek);
    if (static_cast<size_t>(pos) > maxSeek)
    {
        return pos_type(off_type(-1));
    }

    if (which == std::ios_base::in)
    {
        setg(m_buffer, m_buffer + static_cast<size_t>(pos), pptr());                    
    }

    if (which == std::ios_base::out)
    {
        setp(m_buffer + static_cast<size_t>(pos), epptr());
    }

    return pos;
}

bool SimpleStreamBuf::GrowBuffer()
{
    size_t currentSize = m_bufferSize;
    size_t newSize = currentSize * 2;

    char* newBuffer = Aws::NewArray<char>(newSize, SIMPLE_STREAMBUF_ALLOCATION_TAG);
    if(newBuffer == nullptr)
    {
        return false;
    }

    if(currentSize > 0)
    {
        if(m_buffer)
        {
            std::memcpy(newBuffer, m_buffer, currentSize);
        }
        else
        {
            AWS_LOGSTREAM_FATAL(SIMPLE_STREAMBUF_ALLOCATION_TAG, "Unexpected nullptr m_buffer");
        }
    }

    if(m_buffer)
    {
        Aws::DeleteArray<char>(m_buffer);
    }

    m_buffer = newBuffer;
    m_bufferSize = newSize;

    return true;
}

int SimpleStreamBuf::overflow (int c)
{
    auto endOfFile = std::char_traits< char >::eof();
    if(c == endOfFile)
    {
        return endOfFile;
    }

    char* old_begin = m_buffer;

    char *old_pptr = pptr();
    char *old_gptr = gptr();
    char *old_egptr = egptr();

    size_t currentWritePosition = m_bufferSize;

    if(!GrowBuffer())
    {
        return endOfFile;
    }

    char* new_begin = m_buffer;
    char* new_end = new_begin + m_bufferSize;

    setp(new_begin + (old_pptr - old_begin) + 1, new_end);
    setg(new_begin, new_begin + (old_gptr - old_begin), new_begin + (old_egptr - old_begin));

    auto val = std::char_traits< char >::to_char_type(c);
    *(new_begin + currentWritePosition) = val;

    return c;
}

std::streamsize SimpleStreamBuf::xsputn(const char* s, std::streamsize n)
{
    std::streamsize writeCount = 0;
    while(writeCount < n)
    {
        char* current_pptr = pptr();
        char* current_epptr = epptr();

        if (current_pptr < current_epptr)
        {
            std::size_t copySize = (std::min)(static_cast< std::size_t >(n - writeCount),
                                              static_cast< std::size_t >(current_epptr - current_pptr));

            std::memcpy(current_pptr, s + writeCount, copySize);
            writeCount += copySize;
            setp(current_pptr + copySize, current_epptr);
            setg(m_buffer, gptr(), pptr());
        }
        else if (overflow(std::char_traits< char >::to_int_type(*(s + writeCount))) != std::char_traits<char>::eof())
        {
            writeCount++;
        }
        else
        {
            return writeCount;
        }
    }

    return writeCount;
}

Aws::String SimpleStreamBuf::str() const
{
    return Aws::String(m_buffer, pptr());
}

int SimpleStreamBuf::underflow()
{
    if(egptr() != pptr())
    {
        setg(m_buffer, gptr(), pptr());
    }

    if(gptr() != egptr())
    {
        return std::char_traits< char >::to_int_type(*gptr());
    }
    else
    {
        return std::char_traits< char >::eof();
    }
}

void SimpleStreamBuf::str(const Aws::String& value)
{
    char* begin = m_buffer;
    char* end = begin + m_bufferSize;

    setp(begin, end);
    setg(begin, begin, begin);

    xsputn(value.c_str(), value.size());
}

}
}
}
