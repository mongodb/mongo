
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <streambuf>
#include <functional>

namespace Aws
{
    namespace Utils
    {
        namespace Stream
        {
            /**
             * This is a wrapper to perform a hack to write directly to the put area of the underlying streambuf
             */
            class AWS_CORE_API StreamBufProtectedWriter : public std::streambuf
            {
            public:
                StreamBufProtectedWriter() = delete;

                using WriterFunc = std::function<bool(char* dst, uint64_t dstSz, uint64_t& read)>;

                static uint64_t WriteToBuffer(Aws::IOStream& ioStream, const WriterFunc& writerFunc)
                {
                    uint64_t totalRead = 0;

                    while (true)
                    {
                        StreamBufProtectedWriter* pBufferCasted = static_cast<StreamBufProtectedWriter*>(ioStream.rdbuf());
                        bool bufferPresent = pBufferCasted && pBufferCasted->pptr() && (pBufferCasted->pptr() < pBufferCasted->epptr());
                        uint64_t read = 0;
                        bool success = false;
                        if (bufferPresent)
                        {
                            // have access to underlying put ptr.
                            success = WriteDirectlyToPtr(pBufferCasted, writerFunc, read);
                        }
                        else
                        {
                            // can't access underlying buffer, stream buffer maybe be customized to not use put ptr.
                            // or underlying put buffer is simply not initialized yet.
                            success = WriteWithHelperBuffer(ioStream, writerFunc, read);
                        }
                        totalRead += read;
                        if (!success)
                        {
                            break;
                        }

                        if (pBufferCasted && pBufferCasted->pptr() && (pBufferCasted->pptr() >= pBufferCasted->epptr()))
                        {
                            if(!ForceOverflow(ioStream, writerFunc))
                            {
                                break;
                            } else {
                                totalRead++;
                            }
                        }
                    }
                    return totalRead;
                }
            protected:
                static bool ForceOverflow(Aws::IOStream& ioStream, const WriterFunc& writerFunc)
                {
                    char dstChar;
                    uint64_t read = 0;
                    if (writerFunc(&dstChar, 1, read) && read > 0)
                    {
                        ioStream.write(&dstChar, 1);
                        if (ioStream.fail()) {
                            AWS_LOGSTREAM_ERROR("StreamBufProtectedWriter", "Failed to write 1 byte (eof: "
                                    << ioStream.eof() << ", bad: " << ioStream.bad() << ")");
                            return false;
                        }
                        return true;
                    }
                    return false;
                }

                static uint64_t WriteWithHelperBuffer(Aws::IOStream& ioStream, const WriterFunc& writerFunc, uint64_t& read)
                {
                    char tmpBuf[1024];
                    uint64_t tmpBufSz = sizeof(tmpBuf);

                    if(writerFunc(tmpBuf, tmpBufSz, read) && read > 0)
                    {
                        ioStream.write(tmpBuf, read);
                        if (ioStream.fail()) {
                            AWS_LOGSTREAM_ERROR("StreamBufProtectedWriter", "Failed to write " << tmpBufSz
                            << " (eof: " << ioStream.eof() << ", bad: " << ioStream.bad() << ")");
                            return false;
                        }
                        return true;
                    }
                    return false;
                }

                static uint64_t WriteDirectlyToPtr(StreamBufProtectedWriter* pBuffer, const WriterFunc& writerFunc, uint64_t& read)
                {
                    auto dstBegin = pBuffer->pptr();
                    uint64_t dstSz = pBuffer->epptr() - dstBegin;
                    if(writerFunc(dstBegin, dstSz, read) && read > 0)
                    {
                        assert(read <= dstSz);
                        pBuffer->pbump((int) read);
                        return true;
                    }
                    return false;
                }
            };
        }
    }
}
