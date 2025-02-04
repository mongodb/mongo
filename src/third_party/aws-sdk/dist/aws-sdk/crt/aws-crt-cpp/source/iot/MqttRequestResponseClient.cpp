/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iot/MqttRequestResponseClient.h>

#include <aws/crt/mqtt/Mqtt5Client.h>
#include <aws/crt/mqtt/MqttConnection.h>

#include <aws/common/ref_count.h>
#include <aws/common/rw_lock.h>

namespace Aws
{
    namespace Iot
    {
        namespace RequestResponse
        {

            /*
             * RAII wrapper around taking a read lock for a referenced read-write lock.
             *
             * Used to protect the stream from destruction while in a callback or Open() call.  Protects the
             * m_closed field of StreamingOperationImpl.
             */
            class StreamReadLock
            {
              public:
                StreamReadLock(struct aws_rw_lock *lock) : m_lock(lock) { aws_rw_lock_rlock(lock); }

                ~StreamReadLock() { aws_rw_lock_runlock(m_lock); }

              private:
                struct aws_rw_lock *m_lock;
            };

            /*
             * RAII wrapper around taking a *conditional* write lock for a referenced read-write lock.  Protects the
             * m_closed field of StreamingOperationImpl.
             *
             * Used to block callbacks while destruction is triggered.  Only ever used by the stream destructor.
             * We conditionally take the lock because if we're already in the event loop thread we're safe and we
             * are probably (but not guaranteed to be) in a callback.  This prevents deadlock from trying to upgrade
             * an already taken read lock to a write lock, which is not supported by the underlying read-write lock.
             */
            class StreamWriteLock
            {
              public:
                StreamWriteLock(struct aws_rw_lock *lock, struct aws_event_loop *protocolLoop)
                    : m_lock(lock), m_taken(false)
                {
                    if (!aws_event_loop_thread_is_callers_thread(protocolLoop))
                    {
                        m_taken = true;
                        aws_rw_lock_wlock(lock);
                    }
                }

                ~StreamWriteLock()
                {
                    if (m_taken)
                    {
                        aws_rw_lock_wunlock(m_lock);
                    }
                }

              private:
                struct aws_rw_lock *m_lock;

                bool m_taken;
            };

            class StreamingOperationImpl
            {
              public:
                StreamingOperationImpl(
                    struct aws_mqtt_rr_client_operation *stream,
                    const StreamingOperationOptionsInternal &options,
                    struct aws_event_loop *protocolLoop);
                virtual ~StreamingOperationImpl();

                void Open();

                void Close();

                static void OnSubscriptionStatusCallback(
                    enum aws_rr_streaming_subscription_event_type status,
                    int error_code,
                    void *user_data);

                static void OnIncomingPublishCallback(struct aws_byte_cursor payload, void *user_data);

                static void OnTerminatedCallback(void *user_data);

              private:
                StreamingOperationOptionsInternal m_config;

                struct aws_mqtt_rr_client_operation *m_stream;

                struct aws_event_loop *m_protocolLoop;

                struct aws_rw_lock m_lock;

                bool m_closed;
            };

            struct StreamingOperationImplHandle
            {
                StreamingOperationImplHandle() : m_allocator(nullptr), m_impl(nullptr) {}

                Aws::Crt::Allocator *m_allocator;

                std::shared_ptr<StreamingOperationImpl> m_impl;
            };

            StreamingOperationImpl::StreamingOperationImpl(
                struct aws_mqtt_rr_client_operation *stream,
                const StreamingOperationOptionsInternal &options,
                struct aws_event_loop *protocolLoop)
                : m_config(options), m_stream(stream), m_protocolLoop(protocolLoop), m_lock(), m_closed(false)
            {
                aws_rw_lock_init(&m_lock);
            }

            StreamingOperationImpl::~StreamingOperationImpl()
            {
                AWS_FATAL_ASSERT(m_stream == nullptr);
                AWS_FATAL_ASSERT(m_closed);

                aws_rw_lock_clean_up(&m_lock);
            }

            void StreamingOperationImpl::Open()
            {
                {
                    StreamReadLock rlock(&m_lock);

                    if (!m_closed)
                    {
                        aws_mqtt_rr_client_operation_activate(m_stream);
                    }
                }
            }

            void StreamingOperationImpl::Close()
            {
                struct aws_mqtt_rr_client_operation *toRelease = nullptr;

                {
                    StreamWriteLock wlock(&m_lock, m_protocolLoop);

                    if (!m_closed)
                    {
                        m_closed = true;
                        toRelease = m_stream;
                        m_stream = nullptr;
                    }
                }

                if (nullptr != toRelease)
                {
                    aws_mqtt_rr_client_operation_release(toRelease);
                }
            }

            void StreamingOperationImpl::OnSubscriptionStatusCallback(
                enum aws_rr_streaming_subscription_event_type status,
                int error_code,
                void *user_data)
            {

                auto *handle = static_cast<StreamingOperationImplHandle *>(user_data);
                StreamingOperationImpl *impl = handle->m_impl.get();

                {
                    StreamReadLock readLock(&impl->m_lock);

                    if (!impl->m_closed && impl->m_config.subscriptionStatusEventHandler)
                    {
                        SubscriptionStatusEvent event;
                        event.WithType(SubscriptionStatusEventType(status));
                        event.WithErrorCode(error_code);

                        impl->m_config.subscriptionStatusEventHandler(std::move(event));
                    }
                }
            }

            void StreamingOperationImpl::OnIncomingPublishCallback(struct aws_byte_cursor payload, void *user_data)
            {
                auto *handle = static_cast<StreamingOperationImplHandle *>(user_data);
                StreamingOperationImpl *impl = handle->m_impl.get();

                {
                    StreamReadLock readLock(&impl->m_lock);

                    if (!impl->m_closed && impl->m_config.incomingPublishEventHandler)
                    {
                        IncomingPublishEvent event;
                        event.WithPayload(payload);

                        impl->m_config.incomingPublishEventHandler(std::move(event));
                    }
                }
            }

            void StreamingOperationImpl::OnTerminatedCallback(void *user_data)
            {
                auto *handle = static_cast<StreamingOperationImplHandle *>(user_data);

                Aws::Crt::Delete(handle, handle->m_allocator);
            }

            //////////////////////////////////////////////////////////

            class StreamingOperation : public IStreamingOperation
            {
              public:
                static std::shared_ptr<IStreamingOperation> Create(
                    Aws::Crt::Allocator *allocator,
                    const StreamingOperationOptionsInternal &options,
                    struct aws_mqtt_request_response_client *client);

                explicit StreamingOperation(const std::shared_ptr<StreamingOperationImpl> &impl);
                virtual ~StreamingOperation();

                virtual void Open();

              private:
                std::shared_ptr<StreamingOperationImpl> m_impl;
            };

            StreamingOperation::StreamingOperation(const std::shared_ptr<StreamingOperationImpl> &impl) : m_impl(impl)
            {
            }

            std::shared_ptr<IStreamingOperation> StreamingOperation::Create(
                Aws::Crt::Allocator *allocator,
                const StreamingOperationOptionsInternal &options,
                struct aws_mqtt_request_response_client *client)
            {
                auto *implHandle = Aws::Crt::New<StreamingOperationImplHandle>(allocator);

                struct aws_mqtt_streaming_operation_options streamingOptions;
                AWS_ZERO_STRUCT(streamingOptions);
                streamingOptions.topic_filter = options.subscriptionTopicFilter;
                streamingOptions.subscription_status_callback = StreamingOperationImpl::OnSubscriptionStatusCallback;
                streamingOptions.incoming_publish_callback = StreamingOperationImpl::OnIncomingPublishCallback;
                streamingOptions.terminated_callback = StreamingOperationImpl::OnTerminatedCallback;
                streamingOptions.user_data = implHandle;

                struct aws_mqtt_rr_client_operation *stream =
                    aws_mqtt_request_response_client_create_streaming_operation(client, &streamingOptions);
                if (nullptr == stream)
                {
                    Aws::Crt::Delete(implHandle, allocator);
                    return nullptr;
                }

                auto impl = Aws::Crt::MakeShared<StreamingOperationImpl>(
                    allocator, stream, options, aws_mqtt_request_response_client_get_event_loop(client));
                auto streamingOperation = Aws::Crt::MakeShared<StreamingOperation>(allocator, impl);

                implHandle->m_allocator = allocator;
                implHandle->m_impl = impl;

                return streamingOperation;
            }

            StreamingOperation::~StreamingOperation()
            {
                m_impl->Close();
            }

            void StreamingOperation::Open()
            {
                m_impl->Open();
            }

            //////////////////////////////////////////////////////////

            struct IncompleteRequest
            {
                IncompleteRequest() : m_allocator(nullptr), m_handler() {}

                struct aws_allocator *m_allocator;

                UnmodeledResultHandler m_handler;
            };

            static void s_completeRequestWithError(struct IncompleteRequest *incompleteRequest, int errorCode)
            {
                UnmodeledResult result(errorCode);
                incompleteRequest->m_handler(std::move(result));
            }

            static void s_completeRequestWithSuccess(
                struct IncompleteRequest *incompleteRequest,
                const struct aws_byte_cursor *response_topic,
                const struct aws_byte_cursor *payload)
            {
                UnmodeledResponse response;
                response.WithTopic(*response_topic);
                response.WithPayload(*payload);

                UnmodeledResult result(response);
                incompleteRequest->m_handler(std::move(result));
            }

            static void s_onRequestComplete(
                const struct aws_byte_cursor *response_topic,
                const struct aws_byte_cursor *payload,
                int error_code,
                void *user_data)
            {
                auto *incompleteRequest = static_cast<IncompleteRequest *>(user_data);

                if (error_code != AWS_ERROR_SUCCESS)
                {
                    s_completeRequestWithError(incompleteRequest, error_code);
                }
                else
                {
                    s_completeRequestWithSuccess(incompleteRequest, response_topic, payload);
                }

                Aws::Crt::Delete(incompleteRequest, incompleteRequest->m_allocator);
            }

            class MqttRequestResponseClientImpl
            {
              public:
                explicit MqttRequestResponseClientImpl(Aws::Crt::Allocator *allocator) noexcept;
                ~MqttRequestResponseClientImpl();

                void SeatClient(struct aws_mqtt_request_response_client *client);

                void Close() noexcept;

                int SubmitRequest(
                    const aws_mqtt_request_operation_options &requestOptions,
                    UnmodeledResultHandler &&resultHandler) noexcept;

                std::shared_ptr<IStreamingOperation> CreateStream(const StreamingOperationOptionsInternal &options);

                Aws::Crt::Allocator *GetAllocator() const { return m_allocator; }

              private:
                Aws::Crt::Allocator *m_allocator;

                struct aws_mqtt_request_response_client *m_client;
            };

            MqttRequestResponseClientImpl::MqttRequestResponseClientImpl(Aws::Crt::Allocator *allocator) noexcept
                : m_allocator(allocator), m_client(nullptr)
            {
            }

            MqttRequestResponseClientImpl::~MqttRequestResponseClientImpl()
            {
                AWS_FATAL_ASSERT(m_client == nullptr);
            }

            void MqttRequestResponseClientImpl::SeatClient(struct aws_mqtt_request_response_client *client)
            {
                m_client = client;
            }

            void MqttRequestResponseClientImpl::Close() noexcept
            {
                aws_mqtt_request_response_client_release(m_client);
                m_client = nullptr;
            }

            int MqttRequestResponseClientImpl::SubmitRequest(
                const aws_mqtt_request_operation_options &requestOptions,
                UnmodeledResultHandler &&resultHandler) noexcept
            {
                auto *incompleteRequest = Aws::Crt::New<IncompleteRequest>(m_allocator);
                incompleteRequest->m_allocator = m_allocator;
                incompleteRequest->m_handler = std::move(resultHandler);

                struct aws_mqtt_request_operation_options rawOptions = requestOptions;
                rawOptions.completion_callback = s_onRequestComplete;
                rawOptions.user_data = incompleteRequest;

                int result = aws_mqtt_request_response_client_submit_request(m_client, &rawOptions);
                if (result != AWS_OP_SUCCESS)
                {
                    Aws::Crt::Delete(incompleteRequest, incompleteRequest->m_allocator);
                }

                return result;
            }

            std::shared_ptr<IStreamingOperation> MqttRequestResponseClientImpl::CreateStream(
                const StreamingOperationOptionsInternal &options)
            {
                return StreamingOperation::Create(m_allocator, options, m_client);
            }

            //////////////////////////////////////////////////////////

            static void s_onClientTermination(void *user_data)
            {
                auto *impl = static_cast<MqttRequestResponseClientImpl *>(user_data);

                Aws::Crt::Delete(impl, impl->GetAllocator());
            }

            class MqttRequestResponseClient : public IMqttRequestResponseClient
            {
              public:
                explicit MqttRequestResponseClient(MqttRequestResponseClientImpl *impl);
                virtual ~MqttRequestResponseClient();

                int SubmitRequest(
                    const aws_mqtt_request_operation_options &requestOptions,
                    UnmodeledResultHandler &&resultHandler) override;

                std::shared_ptr<IStreamingOperation> CreateStream(
                    const StreamingOperationOptionsInternal &options) override;

              private:
                MqttRequestResponseClientImpl *m_impl;
            };

            int MqttRequestResponseClient::SubmitRequest(
                const aws_mqtt_request_operation_options &requestOptions,
                UnmodeledResultHandler &&resultHandler)
            {
                return m_impl->SubmitRequest(requestOptions, std::move(resultHandler));
            }

            std::shared_ptr<IStreamingOperation> MqttRequestResponseClient::CreateStream(
                const StreamingOperationOptionsInternal &options)
            {
                return m_impl->CreateStream(options);
            }

            MqttRequestResponseClient::MqttRequestResponseClient(MqttRequestResponseClientImpl *impl) : m_impl(impl) {}

            MqttRequestResponseClient::~MqttRequestResponseClient()
            {
                m_impl->Close();
            }

            std::shared_ptr<IMqttRequestResponseClient> NewClientFrom5(
                const Aws::Crt::Mqtt5::Mqtt5Client &protocolClient,
                const RequestResponseClientOptions &options,
                Aws::Crt::Allocator *allocator)
            {
                auto *clientImpl = Aws::Crt::New<MqttRequestResponseClientImpl>(allocator, allocator);

                struct aws_mqtt_request_response_client_options rrClientOptions;
                AWS_ZERO_STRUCT(rrClientOptions);
                rrClientOptions.max_request_response_subscriptions = options.GetMaxRequestResponseSubscriptions();
                rrClientOptions.max_streaming_subscriptions = options.GetMaxStreamingSubscriptions();
                rrClientOptions.operation_timeout_seconds = options.GetOperationTimeoutInSeconds();
                rrClientOptions.terminated_callback = s_onClientTermination;
                rrClientOptions.user_data = clientImpl;

                struct aws_mqtt_request_response_client *rrClient =
                    aws_mqtt_request_response_client_new_from_mqtt5_client(
                        allocator, protocolClient.GetUnderlyingHandle(), &rrClientOptions);
                if (nullptr == rrClient)
                {
                    Aws::Crt::Delete(clientImpl, clientImpl->GetAllocator());
                    return nullptr;
                }

                clientImpl->SeatClient(rrClient);

                return Aws::Crt::MakeShared<MqttRequestResponseClient>(allocator, clientImpl);
            }

            std::shared_ptr<IMqttRequestResponseClient> NewClientFrom311(
                const Aws::Crt::Mqtt::MqttConnection &protocolClient,
                const RequestResponseClientOptions &options,
                Aws::Crt::Allocator *allocator)
            {
                auto *clientImpl = Aws::Crt::New<MqttRequestResponseClientImpl>(allocator, allocator);

                struct aws_mqtt_request_response_client_options rrClientOptions;
                AWS_ZERO_STRUCT(rrClientOptions);
                rrClientOptions.max_request_response_subscriptions = options.GetMaxRequestResponseSubscriptions();
                rrClientOptions.max_streaming_subscriptions = options.GetMaxStreamingSubscriptions();
                rrClientOptions.operation_timeout_seconds = options.GetOperationTimeoutInSeconds();
                rrClientOptions.terminated_callback = s_onClientTermination;
                rrClientOptions.user_data = clientImpl;

                struct aws_mqtt_request_response_client *rrClient =
                    aws_mqtt_request_response_client_new_from_mqtt311_client(
                        allocator, protocolClient.GetUnderlyingConnection(), &rrClientOptions);
                if (nullptr == rrClient)
                {
                    Aws::Crt::Delete(clientImpl, clientImpl->GetAllocator());
                    return nullptr;
                }

                clientImpl->SeatClient(rrClient);

                return Aws::Crt::MakeShared<MqttRequestResponseClient>(allocator, clientImpl);
            }
        } // namespace RequestResponse
    } // namespace Iot
} // namespace Aws
