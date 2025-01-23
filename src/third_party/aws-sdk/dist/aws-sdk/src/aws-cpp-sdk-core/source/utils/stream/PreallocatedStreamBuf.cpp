
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Stream
        {
            PreallocatedStreamBuf::PreallocatedStreamBuf(unsigned char* buffer, uint64_t lengthToRead) :
                m_underlyingBuffer(buffer), m_lengthToRead(lengthToRead)
            {
                char* end = reinterpret_cast<char*>(m_underlyingBuffer + m_lengthToRead);
                char* begin = reinterpret_cast<char*>(m_underlyingBuffer);
                setp(begin, end);
                setg(begin, begin, end);
            }

            PreallocatedStreamBuf::pos_type PreallocatedStreamBuf::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which)
            {
                if (dir == std::ios_base::beg)
                {
                    return seekpos(off, which);
                }
                else if (dir == std::ios_base::end)
                {
                    return seekpos(m_lengthToRead - off, which);
                }
                else if (dir == std::ios_base::cur)
                {
                    if(which == std::ios_base::in)
                    { 
                        return seekpos((gptr() - reinterpret_cast<char*>(m_underlyingBuffer)) + off, which);
                    }
                    else
                    {
                        return seekpos((pptr() - reinterpret_cast<char*>(m_underlyingBuffer)) + off, which);
                    }
                }

                return off_type(-1);
            }

            PreallocatedStreamBuf::pos_type PreallocatedStreamBuf::seekpos(pos_type pos, std::ios_base::openmode which)
            {
                assert(static_cast<size_t>(pos) <= m_lengthToRead);
                if (static_cast<size_t>(pos) > m_lengthToRead)
                {
                    return pos_type(off_type(-1));
                }

                char* end = reinterpret_cast<char*>(m_underlyingBuffer + m_lengthToRead);
                char* begin = reinterpret_cast<char*>(m_underlyingBuffer);

                if (which == std::ios_base::in)
                {
                    setg(begin, begin + static_cast<size_t>(pos), end);
                }

                if (which == std::ios_base::out)
                {
                    setp(begin + static_cast<size_t>(pos), end);
                }

                return pos;
            }
        }
    }
}
