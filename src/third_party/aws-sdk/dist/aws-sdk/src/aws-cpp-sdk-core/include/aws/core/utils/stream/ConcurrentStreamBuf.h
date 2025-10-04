/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/auth/AWSAuthSigner.h>
#include <aws/common/array_list.h>

#include <mutex>
#include <condition_variable>
#include <streambuf>
#include <ios>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Stream
        {
            /**
             * A thread-safe streambuf implementation that allows simultaneous reading and writing.
             * NOTE: iostreams maintain state for readers and writers. This means that you can have at most two
             * concurrent threads, one for reading and one for writing. Multiple readers or multiple writers are not
             * thread-safe and will result in race-conditions.
             */
            class AWS_CORE_API ConcurrentStreamBuf : public std::streambuf
            {
            public:

                explicit ConcurrentStreamBuf(size_t bufferLength = 8 * 1024);

                void SetEofInput(Aws::IOStream* pStreamToClose = nullptr);
                void CloseStream();

                /**
                 * Blocks the current thread until all submitted data is consumed.
                 * Returns false on timeout, and true if GetArea and back buffer are empty.
                 */
                bool WaitForDrain(int64_t timeoutMs);

            protected:
                std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
                std::streampos seekpos(std::streampos pos, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;

                int underflow() override;
                int uflow() override;
                int overflow(int ch) override;
                int sync() override;
                std::streamsize showmanyc() override;

                void FlushPutArea();

            private:
                Aws::Vector<unsigned char> m_getArea;
                Aws::Vector<unsigned char> m_putArea;
                Aws::Vector<unsigned char> m_backbuf; // used to shuttle data from the put area to the get area
                std::mutex m_lock; // synchronize access to the common backbuffer
                std::condition_variable m_signal;
                bool m_eofInput = false;
                bool m_eofOutput = false;
                Aws::IOStream* m_pStreamToClose = nullptr;
            };
        }
    }
}
