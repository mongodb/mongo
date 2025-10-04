/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Aws
{
    namespace Utils
    {
        /**
         * A simple wrapper over std::atomic<size_t> that captures atomic by reference and
         *   increases-decreases it's count in constructor-destructor.
         *   Optionally notifies all on conditional variable pointer if set and counter reaches 0.
         */
        class AWS_CORE_API RAIICounter final
        {
        public:
            RAIICounter(std::atomic<size_t>& iCount, std::condition_variable* cv = nullptr);
            ~RAIICounter();

            RAIICounter(const RAIICounter&) = delete;
            RAIICounter(RAIICounter&&) = delete;
            RAIICounter& operator=(const RAIICounter&) = delete;
            RAIICounter& operator=(RAIICounter&&) = delete;

        protected:
            std::atomic<size_t>& m_count;
            std::condition_variable* m_cv;
        };
    }
}

