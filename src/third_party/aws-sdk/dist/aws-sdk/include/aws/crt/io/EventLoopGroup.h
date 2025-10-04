#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Types.h>

#include <aws/io/event_loop.h>

namespace Aws
{
    namespace Crt
    {
        namespace Io
        {
            /**
             * A collection of event loops.
             *
             * An event-loop is a thread for doing async work, such as I/O. Classes that need to do async work will ask
             * the EventLoopGroup for an event-loop to use.
             *
             * The number of threads used depends on your use-case. IF you
             * have a maximum of less than a few hundred connections 1 thread is the ideal
             * threadCount.
             *
             * There should only be one instance of an EventLoopGroup per application and it
             * should be passed to all network clients. One exception to this is if you
             * want to peg different types of IO to different threads. In that case, you
             * may want to have one event loop group dedicated to one IO activity and another
             * dedicated to another type.
             */
            class AWS_CRT_CPP_API EventLoopGroup final
            {
              public:
                /**
                 * @param threadCount: The number of event-loops to create, default will be 0, which will create one for
                 * each processor on the machine.
                 * @param allocator memory allocator to use.
                 */
                EventLoopGroup(uint16_t threadCount = 0, Allocator *allocator = ApiAllocator()) noexcept;
                /**
                 * @param cpuGroup: The CPU group (e.g. NUMA nodes) that all hardware threads are pinned to.
                 * @param threadCount: The number of event-loops to create, default will be 0, which will create one for
                 * each processor on the machine.
                 * @param allocator memory allocator to use.
                 */
                EventLoopGroup(uint16_t cpuGroup, uint16_t threadCount, Allocator *allocator = ApiAllocator()) noexcept;
                ~EventLoopGroup();
                EventLoopGroup(const EventLoopGroup &) = delete;
                EventLoopGroup(EventLoopGroup &&) noexcept;
                EventLoopGroup &operator=(const EventLoopGroup &) = delete;
                EventLoopGroup &operator=(EventLoopGroup &&) noexcept;

                /**
                 * @return true if the instance is in a valid state, false otherwise.
                 */
                operator bool() const;

                /**
                 * @return the value of the last aws error encountered by operations on this instance.
                 */
                int LastError() const;

                /// @private
                aws_event_loop_group *GetUnderlyingHandle() noexcept;

              private:
                aws_event_loop_group *m_eventLoopGroup;
                int m_lastError;
            };
        } // namespace Io

    } // namespace Crt
} // namespace Aws
