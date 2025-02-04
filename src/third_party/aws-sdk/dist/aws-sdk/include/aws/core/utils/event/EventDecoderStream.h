/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/event/EventStreamBuf.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            extern AWS_CORE_API const size_t DEFAULT_BUF_SIZE;

            /**
             * A buffered I/O stream that binary-decodes the bits written to it according to the AWS event-stream spec.
             * The decoding process will result in invoking callbacks on the handler assigned to the decoder parameter.
             */
            class AWS_CORE_API EventDecoderStream : public Aws::IOStream
            {
            public:
                /**
                 * Creates a stream for decoding events sent by the service.
                 * @param decoder decodes the stream from server side, so as to invoke related callback functions.
                 * @param eventStreamBufLength The length of the underlying buffer.
                 */
                EventDecoderStream(EventStreamDecoder& decoder, size_t bufferSize = DEFAULT_BUF_SIZE);

            private:
                EventDecoderStream(const EventDecoderStream&) = delete;
                EventDecoderStream(EventDecoderStream&&) = delete;
                EventDecoderStream& operator=(const EventDecoderStream&) = delete;
                EventDecoderStream& operator=(EventDecoderStream&&) = delete;

                EventStreamBuf m_eventStreamBuf;
            };
        }
    }
}
