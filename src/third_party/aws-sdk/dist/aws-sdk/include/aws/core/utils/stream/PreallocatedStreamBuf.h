
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/Array.h>
#include <streambuf>

namespace Aws
{
    namespace Utils
    {
        namespace Stream
        {
            /**
             * this is a stream buf to use with std::iostream that uses a preallocated buffer under the hood.
             */
            class AWS_CORE_API PreallocatedStreamBuf : public std::streambuf
            {
            public:
                /**
                 * Initialize the stream buffer with a pointer to your buffer. This class never takes ownership
                 * of the buffer. It is your responsibility to delete it once the stream is no longer in use.
                 * @param buffer buffer to initialize from.
                 * @param lengthToRead length in bytes to actually use in the buffer (e.g. you have a 1kb buffer, but only want the stream
                 * to see 500 b of it.
                 */
                PreallocatedStreamBuf(unsigned char* buffer, uint64_t lengthToRead);

                PreallocatedStreamBuf(const PreallocatedStreamBuf&) = delete;
                PreallocatedStreamBuf& operator=(const PreallocatedStreamBuf&) = delete;

                PreallocatedStreamBuf(PreallocatedStreamBuf&& toMove) = delete;
                PreallocatedStreamBuf& operator=(PreallocatedStreamBuf&&) = delete;

                /**
                 * Get the buffer that is being used by the stream buffer.
                 * @return Pointer to the underlying buffer (probably for a Aws::Delete() call).
                 */
                unsigned char* GetBuffer() { return m_underlyingBuffer; }

            protected:
                pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
                pos_type seekpos(pos_type pos, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;

            private:
                unsigned char* m_underlyingBuffer;
                const uint64_t m_lengthToRead;
            };
        }
    }
}
