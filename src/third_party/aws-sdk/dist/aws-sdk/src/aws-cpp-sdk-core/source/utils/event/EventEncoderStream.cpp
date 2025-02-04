/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/event/EventEncoderStream.h>
#include <iostream>

#include <aws/core/utils/HashingUtils.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            EventEncoderStream::EventEncoderStream(size_t bufferSize) :
                Aws::IOStream(&m_streambuf),
                m_streambuf(bufferSize)
            {
            }

            bool EventEncoderStream::WaitForDrain(int64_t timeoutMs)
            {
                return m_streambuf.WaitForDrain(timeoutMs);
            }

            EventEncoderStream& EventEncoderStream::WriteEvent(const Aws::Utils::Event::Message& msg)
            {
                auto bits = m_encoder.EncodeAndSign(msg);

                AWS_LOGSTREAM_TRACE("EventEncoderStream::WriteEvent", "Encoded event (base64 encoded): " <<
                                    Aws::Utils::HashingUtils::Base64Encode(Aws::Utils::ByteBuffer(bits.data(), bits.size())));

                // write buffer to underlying rdbuf (ConcurrentStreamBuf), this may call overflow()
                // and block until data is consumed by HTTP Client
                write(reinterpret_cast<char*>(bits.data()), bits.size());
                // force flushing ConcurrentStreamBuf to move data from PutArea to the back buffer
                // so that consuming HTTP Client will have data to send
                flush();
                return *this;
            }
        }
    }
}
