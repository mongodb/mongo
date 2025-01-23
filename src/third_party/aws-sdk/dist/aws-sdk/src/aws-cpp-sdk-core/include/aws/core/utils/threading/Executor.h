/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#if !defined(AWS_EXECUTOR_H)
#define AWS_EXECUTOR_H

#include <aws/core/Core_EXPORTS.h>

#include <functional>

namespace Aws
{
    namespace Utils
    {
        namespace Threading
        {
            /**
            * Interface for implementing an Executor, to implement a custom thread execution strategy, inherit from this class
            * and override SubmitToThread().
            */
            class AWS_CORE_API Executor
            {
            public:                
                virtual ~Executor() = default;

                /**
                 * Send function and its arguments to the SubmitToThread function.
                 */
                template<class Fn, class ... Args>
                bool Submit(Fn&& fn, Args&& ... args)
                {
                    std::function<void()> callable{ std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...) };
                    return SubmitToThread(std::move(callable));
                }

                /* explicit _overload_ of the template function above to avoid template bloat */
                bool Submit(std::function<void()>&& callable)
                {
                    return SubmitToThread(std::move(callable));
                }

                /**
                 * Call to wait until all tasks have finished.
                 */
                virtual void WaitUntilStopped() { return; };

            protected:
                /**
                * To implement your own executor implementation, then simply subclass Executor and implement this method.
                */
                virtual bool SubmitToThread(std::function<void()>&&) = 0;
            };
        } // namespace Threading
    } // namespace Utils
} // namespace Aws

// TODO: remove on a next minor API bump from 1.11.x
#endif // !defined(AWS_EXECUTOR_H)
#include <aws/core/utils/threading/DefaultExecutor.h>
#include <aws/core/utils/threading/PooledThreadExecutor.h>