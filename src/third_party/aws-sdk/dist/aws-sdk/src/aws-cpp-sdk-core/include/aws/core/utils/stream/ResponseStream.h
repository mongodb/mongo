/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>

namespace Aws
{
    namespace Utils
    {
        namespace Stream
        {
            /**
             * Encapsulates and manages ownership of custom response streams. This is a move only type.
             */
            class AWS_CORE_API ResponseStream
            {
            public:
                /**
                 * sets underlying stream to nullptr
                 */
                ResponseStream();
                /**
                 * moves the underlying stream
                 */
                ResponseStream(ResponseStream&&);
                /**
                 * Uses factory to allocate underlying stream
                 */
                ResponseStream(const Aws::IOStreamFactory& factory);
                /**
                 * Takes ownership of an underlying stream.
                 */
                ResponseStream(IOStream* underlyingStreamToManage);
                ResponseStream(const ResponseStream&) = delete;
                ~ResponseStream();

                /**
                 * moves the underlying stream
                 */
                ResponseStream& operator=(ResponseStream&&);
                ResponseStream& operator=(const ResponseStream&) = delete;

                /**
                 * Gives access to underlying stream, but keep in mind that this changes state of the stream
                 */
                Aws::IOStream& GetUnderlyingStream() const;

            private:
                void ReleaseStream();
                void RegisterStream();
                void DeregisterStream();

                Aws::IOStream* m_underlyingStream = nullptr;

                static const int xindex;
                static void StreamCallback(Aws::IOStream::event evt, std::ios_base& str, int idx);
            };

            /**
             * A default IOStream for ResponseStream.
             */
            class AWS_CORE_API DefaultUnderlyingStream : public Aws::IOStream
            {
            public:
                using Base = Aws::IOStream;

                DefaultUnderlyingStream();
                DefaultUnderlyingStream(Aws::UniquePtr<std::streambuf> buf);
                virtual ~DefaultUnderlyingStream();
            };

            AWS_CORE_API Aws::IOStream* DefaultResponseStreamFactoryMethod();

        } //namespace Stream
    } //namespace Utils
} //namespace Aws
