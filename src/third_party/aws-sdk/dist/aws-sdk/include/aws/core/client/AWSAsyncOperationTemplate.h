/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/threading/Executor.h>
#include <functional>
#include <future>

namespace Aws
{
namespace Client
{
    /**
     * A template function that is used to create an Async Operation function body for AWS Operations
     */
    template<typename ClientT,
             typename RequestT,
             typename HandlerT,
             typename HandlerContextT,
             typename OperationFuncT,
             typename ExecutorT>
    inline void AWS_CORE_LOCAL MakeAsyncOperation(OperationFuncT&& operationFunc,
                                   const ClientT* clientThis,
                                   const RequestT& request,
                                   const HandlerT& handler,
                                   const HandlerContextT& context,
                                   ExecutorT* pExecutor)
    {
        std::function<void()> asyncTask =
            [operationFunc, clientThis, request, handler, context]() // note capture by value
            {
                handler(clientThis,
                        request,
                        (clientThis->*operationFunc)(request),
                        context);
            };

        pExecutor->Submit(std::move(asyncTask));
    }

    /**
     * A template function that is used to create an Async Operation function body for AWS Streaming Operations
     *   The only difference compared to a regular non-streaming Operation is that
     *   the request is passed by non-const reference, therefore virtual copy constructor is not needed.
     *   However, caller code must ensure the life time of the request object is maintained during the Async execution.
     */
    template<typename ClientT,
             typename RequestT,
             typename HandlerT,
             typename HandlerContextT,
             typename OperationFuncT,
             typename ExecutorT>
    inline void AWS_CORE_LOCAL MakeAsyncStreamingOperation(OperationFuncT&& operationFunc,
                                            const ClientT* clientThis,
                                            RequestT& request, // note non-const ref
                                            const HandlerT& handler,
                                            const HandlerContextT& context,
                                            ExecutorT* pExecutor)
    {
        std::function<void()> asyncTask =
            [operationFunc, clientThis, &request, handler, context]() // note capture by ref
            {
                handler(clientThis,
                        request,
                        (clientThis->*operationFunc)(request),
                        context);
            };

        pExecutor->Submit(std::move(asyncTask));
    }

    /**
     * A template function to create an Async Operation function body for AWS Operation without a request on input.
     */
    template<typename ClientT,
             typename HandlerT,
             typename HandlerContextT,
             typename OperationFuncT,
             typename ExecutorT>
    inline void AWS_CORE_LOCAL MakeAsyncOperation(OperationFuncT&& operationFunc,
                                            const ClientT* clientThis,
                                            const HandlerT& handler,
                                            const HandlerContextT& context,
                                            ExecutorT* pExecutor)
    {
        std::function<void()> asyncTask =
            [operationFunc, clientThis, handler, context]()
            {
                handler(clientThis,
                        (clientThis->*operationFunc)(),
                        context);
            };

        pExecutor->Submit(std::move(asyncTask));
    }

    /**
     * A template function that is used to create a Callable Operation function body for AWS Operations
     */
    template<typename ClientT,
             typename RequestT,
             typename OperationFuncT,
             typename ExecutorT>
    inline auto AWS_CORE_LOCAL MakeCallableOperation(const char* ALLOCATION_TAG,
                                      OperationFuncT&& operationFunc,
                                      const ClientT* clientThis,
                                      const RequestT& request,
                                      ExecutorT* pExecutor) -> std::future<decltype((clientThis->*operationFunc)(request))>
    {
        using OperationOutcomeT = decltype((clientThis->*operationFunc)(request));

        auto task = Aws::MakeShared< std::packaged_task< OperationOutcomeT() > >(
                ALLOCATION_TAG,
                [clientThis, operationFunc, request]() // note capture by value
                {
                    auto futureOutcome = (clientThis->*operationFunc)(request);
                    return futureOutcome;
                } );

        std::function<void()> packagedFunction =
                [task]() { (*task)(); };
        pExecutor->Submit(std::move(packagedFunction));
        return task->get_future();
    }

    /**
     * A template function that is used to create a Callable Operation function body for AWS Streaming Operations
     *   The only difference compared to a regular non-streaming Operation is that
     *   the request is passed by non-const reference, therefore virtual copy constructor is not needed.
     *   However, caller code must ensure the life time of the request object is maintained during the Async execution.
     */
    template<typename ClientT,
             typename RequestT,
             typename OperationFuncT,
             typename ExecutorT>
    inline auto AWS_CORE_LOCAL MakeCallableStreamingOperation(const char* ALLOCATION_TAG,
                                               OperationFuncT&& operationFunc,
                                               const ClientT* clientThis,
                                               RequestT& request,  // note non-const ref
                                               ExecutorT* pExecutor) -> std::future<decltype((clientThis->*operationFunc)(request))>
    {
        using OperationOutcomeT = decltype((clientThis->*operationFunc)(request));

        auto task = Aws::MakeShared< std::packaged_task< OperationOutcomeT() > >(
                ALLOCATION_TAG,
                [clientThis, operationFunc, &request]()  // note capture by ref
                {
                    return (clientThis->*operationFunc)(request);
                } );

        std::function<void()> packagedFunction =
                [task]() { (*task)(); };
        pExecutor->Submit(std::move(packagedFunction));
        return task->get_future();
    }

    /**
     * A template function that is used to create a Callable Operation function body for AWS Operation without a request on input.
     */
    template<typename ClientT,
             typename OperationFuncT,
             typename ExecutorT>
    inline auto AWS_CORE_LOCAL MakeCallableOperation(const char* ALLOCATION_TAG,
                                               OperationFuncT&& operationFunc,
                                               const ClientT* clientThis,
                                               ExecutorT* pExecutor) -> std::future<decltype((clientThis->*operationFunc)())>
    {
        using OperationOutcomeT = decltype((clientThis->*operationFunc)());

        auto task = Aws::MakeShared< std::packaged_task< OperationOutcomeT() > >(
                ALLOCATION_TAG,
                [clientThis, operationFunc]()
                {
                    return (clientThis->*operationFunc)();
                } );

        std::function<void()> packagedFunction =
                [task]() { (*task)(); };
        pExecutor->Submit(std::move(packagedFunction));
        return task->get_future();
    }
} // namespace Client
} // namespace Aws


