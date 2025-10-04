
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
#include <streambuf>

namespace Aws
{
namespace Utils
{
namespace Stream
{
    /**
    * A replacement for std::stringbuf when using Android and gnustl together
    */
    class AWS_CORE_API SimpleStreamBuf : public std::streambuf
    {
        public:

            using base = std::streambuf;

            SimpleStreamBuf();
            explicit SimpleStreamBuf(const Aws::String& value);

            SimpleStreamBuf(const SimpleStreamBuf&) = delete;
            SimpleStreamBuf& operator=(const SimpleStreamBuf&) = delete;

            SimpleStreamBuf(SimpleStreamBuf&& toMove) = delete;
            SimpleStreamBuf& operator=(SimpleStreamBuf&&) = delete;

            virtual ~SimpleStreamBuf();

            Aws::String str() const;
            void str(const Aws::String& value);

            void swap(SimpleStreamBuf& rhs);

        protected:
            virtual std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;
            virtual std::streampos seekpos(std::streampos pos, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override;

            virtual int overflow (int c = EOF) override;
            virtual int underflow() override;

            virtual std::streamsize xsputn(const char* s, std::streamsize n) override;

        private:

            bool GrowBuffer();

            char* m_buffer;
            size_t m_bufferSize;
    };

}
}
}
