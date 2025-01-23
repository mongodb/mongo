/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/utils/event/EventDecoderStream.h>
#include <iostream>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            EventDecoderStream::EventDecoderStream(EventStreamDecoder& decoder, size_t bufferSize) :
                Aws::IOStream(&m_eventStreamBuf),
                m_eventStreamBuf(decoder, bufferSize)

            {
            }
        }
    }
}
