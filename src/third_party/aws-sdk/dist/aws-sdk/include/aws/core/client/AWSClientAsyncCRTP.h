/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/client/AWSAsyncOperationTemplate.h>
#include <aws/core/utils/logging/ErrorMacros.h>
#include <aws/core/utils/component-registry/ComponentRegistry.h>

namespace Aws
{
namespace Client
{
    class AsyncCallerContext;

    /**
     * A helper to determine if AWS Operation is EventStream-enabled or not (based on const-ness of the request)
    */
    template<typename T>
    struct AWS_CORE_LOCAL IsEventStreamOperation : IsEventStreamOperation<decltype(&T::operator())> {};

    template<typename ReturnT, typename ClassT, typename RequestT>
    struct AWS_CORE_LOCAL IsEventStreamOperation<ReturnT(ClassT::*)(RequestT) const>
    {
        static const bool value = !std::is_const<typename std::remove_reference<RequestT>::type>::value;
    };

    template<typename ReturnT, typename ClassT>
    struct AWS_CORE_LOCAL IsEventStreamOperation<ReturnT(ClassT::*)() const>
    {
        static const bool value = false;
    };


    /**
     * A CRTP-base class template that is used to add template methods to call AWS Operations in parallel using ThreadExecutor
     * An Aws<Service>Client is going to inherit from this class and will get methods below available.
    */
    template <typename AwsServiceClientT>
    class ClientWithAsyncTemplateMethods
    {
    public:
        ClientWithAsyncTemplateMethods()
         : m_isInitialized(true),
           m_operationsProcessed(0)
        {
            AwsServiceClientT* pThis = static_cast<AwsServiceClientT*>(this);
            Aws::Utils::ComponentRegistry::RegisterComponent(AwsServiceClientT::GetServiceName(),
                                                             pThis,
                                                             &AwsServiceClientT::ShutdownSdkClient);

        }

        ClientWithAsyncTemplateMethods(const ClientWithAsyncTemplateMethods& other)
         : m_isInitialized(other.m_isInitialized.load()),
           m_operationsProcessed(0)
        {
            AwsServiceClientT* pThis = static_cast<AwsServiceClientT*>(this);
            Aws::Utils::ComponentRegistry::RegisterComponent(AwsServiceClientT::GetServiceName(),
                                                             pThis,
                                                             &AwsServiceClientT::ShutdownSdkClient);
        }

        ClientWithAsyncTemplateMethods& operator=(const ClientWithAsyncTemplateMethods& other)
        {
            if (&other != this)
            {
                ShutdownSdkClient(static_cast<AwsServiceClientT*>(this));
                m_operationsProcessed = 0;
                m_isInitialized = other.m_isInitialized.load();
            }

            return *this;
        }

        virtual ~ClientWithAsyncTemplateMethods()
        {
            AwsServiceClientT* pClient = static_cast<AwsServiceClientT*>(this);
            Aws::Utils::ComponentRegistry::DeRegisterComponent(pClient);
        }

        /**
         * A callback static method to terminate client (i.e. to free dynamic resources and prevent further processing)
         * @param pThis, a void* pointer that points to AWS SDK Service Client, such as "Aws::S3::S3Client"
         * @param timeoutMs, a timeout (in ms) that this method will wait for currently running operations to complete.
         *                    "-1" represents "use clientConfiguration.requestTimeoutMs" value.
         */
        static void ShutdownSdkClient(void* pThis, int64_t timeoutMs = -1)
        {
            AwsServiceClientT* pClient = reinterpret_cast<AwsServiceClientT*>(pThis);
            AWS_CHECK_PTR(AwsServiceClientT::GetServiceName(), pClient);
            if(!pClient->m_isInitialized)
            {
                return;
            }

            std::unique_lock<std::mutex> lock(pClient->m_shutdownMutex);

            pClient->m_isInitialized = false;
            if (pClient->GetHttpClient().use_count() == 1)
            {
                pClient->DisableRequestProcessing();
            }

            if (timeoutMs == -1)
            {
                timeoutMs = pClient->m_clientConfiguration.requestTimeoutMs;
            }
            pClient->m_shutdownSignal.wait_for(lock,
                                      std::chrono::milliseconds(timeoutMs),
                                      [&](){ return pClient->m_operationsProcessed.load() == 0; });

            if (pClient->m_operationsProcessed.load())
            {
                AWS_LOGSTREAM_FATAL(AwsServiceClientT::GetAllocationTag(), "Service client "
                    << AwsServiceClientT::GetServiceName() << " is shutting down while async tasks are present.");
            }

            pClient->m_clientConfiguration.executor.reset();
            pClient->m_clientConfiguration.retryStrategy.reset();
            pClient->m_endpointProvider.reset();
        }

        /**
         * A template to submit a AwsServiceClient regular operation method for async execution.
         * This template method copies and queues the request into a thread executor and triggers associated callback when operation has finished.
        */
        template<typename RequestT, typename HandlerT, typename OperationFuncT, typename std::enable_if<!IsEventStreamOperation<OperationFuncT>::value, int>::type = 0>
        void SubmitAsync(OperationFuncT operationFunc,
                         const RequestT& request,
                         const HandlerT& handler,
                         const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            Aws::Client::MakeAsyncOperation(operationFunc, clientThis, request, handler, context, clientThis->m_clientConfiguration.executor.get());
        }

        /**
         * A template to submit a AwsServiceClient event stream enabled operation method for async execution.
         * This template method queues the original request object into a thread executor and triggers associated callback when operation has finished.
         * It is caller's responsibility to ensure the lifetime of the original request object for a duration of the async execution.
        */
        template<typename RequestT, typename HandlerT, typename OperationFuncT, typename std::enable_if<IsEventStreamOperation<OperationFuncT>::value, int>::type = 0>
        void SubmitAsync(OperationFuncT operationFunc,
                         RequestT& request, // note non-const ref
                         const HandlerT& handler,
                         const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            Aws::Client::MakeAsyncStreamingOperation(operationFunc, clientThis, request, handler, context, clientThis->m_clientConfiguration.executor.get());
        }

        /**
         * A template to submit a AwsServiceClient regular operation method without arguments for async execution.
         * This template method submits a task into a thread executor and triggers associated callback when operation has finished.
        */
        template<typename HandlerT, typename OperationFuncT>
        void SubmitAsync(OperationFuncT operationFunc,
                         const HandlerT& handler,
                         const std::shared_ptr<const Aws::Client::AsyncCallerContext>& context = nullptr) const
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            Aws::Client::MakeAsyncOperation(operationFunc, clientThis, handler, context, clientThis->m_clientConfiguration.executor.get());
        }

        /**
         * A template to submit a AwsServiceClient regular operation method for async execution that returns a future<OperationOutcome> object.
         * This template method copies and queues the request into a thread executor and returns a future<OperationOutcome> object when operation has finished.
         */
        template<typename RequestT, typename OperationFuncT, typename std::enable_if<!IsEventStreamOperation<OperationFuncT>::value, int>::type = 0>
        auto SubmitCallable(OperationFuncT operationFunc,
                            const RequestT& request) const
            -> std::future<decltype((static_cast<const AwsServiceClientT*>(nullptr)->*operationFunc)(request))>
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            return Aws::Client::MakeCallableOperation(AwsServiceClientT::GetAllocationTag(), operationFunc, clientThis, request, clientThis->m_clientConfiguration.executor.get());
        }

        /**
         * A template to submit a AwsServiceClient event stream enabled operation method for async execution that returns a future<OperationOutcome> object.
         * This template method queues the original request into a thread executor and returns a future<OperationOutcome> object when operation has finished.
         * It is caller's responsibility to ensure the lifetime of the original request object for a duration of the async execution.
         */
        template<typename RequestT, typename OperationFuncT, typename std::enable_if<IsEventStreamOperation<OperationFuncT>::value, int>::type = 0>
        auto SubmitCallable(OperationFuncT operationFunc, /*note non-const ref*/ RequestT& request) const
            -> std::future<decltype((static_cast<const AwsServiceClientT*>(nullptr)->*operationFunc)(request))>
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            return Aws::Client::MakeCallableStreamingOperation(AwsServiceClientT::GetAllocationTag(), operationFunc, clientThis, request, clientThis->m_clientConfiguration.executor.get());
        }

        /**
         * A template to submit a AwsServiceClient regular operation without request argument for
         *   an async execution that returns a future<OperationOutcome> object.
         * This template method copies and queues the request into a thread executor and returns a future<OperationOutcome> object when operation has finished.
         */
        template<typename OperationFuncT>
        auto SubmitCallable(OperationFuncT operationFunc) const
            -> std::future<decltype((static_cast<const AwsServiceClientT*>(nullptr)->*operationFunc)())>
        {
            const AwsServiceClientT* clientThis = static_cast<const AwsServiceClientT*>(this);
            return Aws::Client::MakeCallableOperation(AwsServiceClientT::GetAllocationTag(), operationFunc, clientThis, clientThis->m_clientConfiguration.executor.get());
        }
    protected:
        std::atomic<bool> m_isInitialized;
        mutable std::atomic<size_t> m_operationsProcessed;
        mutable std::condition_variable m_shutdownSignal;
        mutable std::mutex m_shutdownMutex;

        // TODO: track scheduled tasks
        // std::atomic<size_t> m_operationsScheduled;
    };
} // namespace Client
} // namespace Aws
