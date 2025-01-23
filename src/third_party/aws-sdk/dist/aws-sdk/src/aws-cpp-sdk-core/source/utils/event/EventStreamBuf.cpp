/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/event/EventStreamBuf.h>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            const size_t DEFAULT_BUF_SIZE = 8096;

            EventStreamBuf::EventStreamBuf(EventStreamDecoder& decoder, size_t bufferLength) :
                m_byteBuffer(bufferLength),
                m_bufferLength(bufferLength),
                m_decoder(decoder)
            {
                assert(decoder);
                char* begin = reinterpret_cast<char*>(m_byteBuffer.GetUnderlyingData());
                char* end = begin + bufferLength - 1;

                setp(begin, end);
                setg(begin, begin, begin);
            }

            EventStreamBuf::~EventStreamBuf()
            {
                if (m_decoder)
                {
                    writeToDecoder();
                }
            }

            void EventStreamBuf::writeToDecoder()
            {
                if (pptr() > pbase())
                {
                    assert(epptr() >= pptr()); // check that we are in a put area
                    size_t length = static_cast<size_t>(pptr() - pbase());
                    m_decoder.Pump(m_byteBuffer, length);

                    if (!m_decoder)
                    {
                        m_err.write(reinterpret_cast<char*>(m_byteBuffer.GetUnderlyingData()), length);
                        if (m_err.fail()) {
                            AWS_LOGSTREAM_ERROR("EventStreamBuf",
                                                "Failed to write " << length << " (eof: " << m_err.eof() << ", bad: " << m_err.bad() << ")");
                        }
                    }
                    else
                    {
                        pbump(-static_cast<int>(length));
                    }
                }
            }

            std::streampos EventStreamBuf::seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode which)
            {
                if (dir == std::ios_base::beg)
                {
                    return seekpos(off, which);
                }
                else if (dir == std::ios_base::end)
                {
                    return seekpos(m_bufferLength - 1 - off, which);
                }
                else if (dir == std::ios_base::cur)
                {
                    if (which == std::ios_base::in)
                    {
                        return seekpos((gptr() - (char*)m_byteBuffer.GetUnderlyingData()) + off, which);
                    }
                    if (which == std::ios_base::out)
                    {
                        return seekpos((pptr() - (char*)m_byteBuffer.GetUnderlyingData()) + off, which);
                    }
                }

                return std::streamoff(-1);
            }

            std::streampos EventStreamBuf::seekpos(std::streampos pos, std::ios_base::openmode which)
            {
                assert(static_cast<size_t>(pos) <= m_bufferLength);
                if (static_cast<size_t>(pos) > m_bufferLength)
                {
                    return std::streampos(std::streamoff(-1));
                }

                if (which == std::ios_base::in)
                {
                    m_err.seekg(pos);
                    return m_err.tellg();
                }

                if (which == std::ios_base::out)
                {
                    return pos;
                }

                return std::streampos(std::streamoff(-1));
            }

            int EventStreamBuf::underflow()
            {
                if (!m_err || m_err.eof() || m_decoder)
                {
                    return std::char_traits<char>::eof();
                }

                m_err.flush();
                m_err.read(reinterpret_cast<char*>(m_byteBuffer.GetUnderlyingData()), m_byteBuffer.GetLength());

                char* begin = reinterpret_cast<char*>(m_byteBuffer.GetUnderlyingData());
                setg(begin, begin, begin + m_err.gcount());
                return std::char_traits<char>::to_int_type(*gptr());
            }

            int EventStreamBuf::overflow(int ch)
            {
                auto eof = std::char_traits<char>::eof();

                if (m_decoder)
                {
                    if (pptr() == epptr()) // always the case by C++ standard, but someone may inherit from this class
                    {
                        writeToDecoder();
                    }
                    // writeToDecoder had to take some data from the put buffer but failed
                    if (pptr() == epptr())
                    {
                        AWS_LOGSTREAM_ERROR("EventStreamBuf", "Failed to decode EventStream event on char with int value: " << ch);
                        // let's just reset the put buffer and move on.
                        setp(pbase(), epptr() - 1);
                    }

                    // put char to the put area and advance current pptr
                    if (ch != eof)
                    {
                        *pptr() = (char)ch;
                        pbump(1);
                    }

                    return ch;
                }

                return eof;
            }

            int EventStreamBuf::sync()
            {
                if (m_decoder)
                {
                    writeToDecoder();
                }

                return 0;
            }
        }
    }
}
