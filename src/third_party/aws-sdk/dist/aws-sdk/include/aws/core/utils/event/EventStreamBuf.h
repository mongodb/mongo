/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <ios>
#include <aws/core/utils/event/EventStreamDecoder.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            extern AWS_CORE_API const size_t DEFAULT_BUF_SIZE;

            /**
             * Derived from std::streambuf, used as the underlying buffer for EventStream.
             * Handle the payload from server side and pass data to underlying decoder.
             */
            class AWS_CORE_API EventStreamBuf : public std::streambuf
            {
            public:
                /**
                 * @param decoder decodes the stream from server side, so as to invoke related callback functions.
                 * @param bufferSize The length of buffer, will be 1024 bytes by default.
                 */
                EventStreamBuf(EventStreamDecoder& decoder, size_t bufferLength = DEFAULT_BUF_SIZE);
                virtual ~EventStreamBuf();

            protected:
                std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
                std::streampos seekpos(std::streampos pos, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;

                int underflow() override;
                int overflow(int ch) override;
                int sync() override;

            private:
                void writeToDecoder();

                ByteBuffer m_byteBuffer;
                size_t m_bufferLength;
                Aws::StringStream m_err;
                EventStreamDecoder& m_decoder;
            };
        }
    }
}
