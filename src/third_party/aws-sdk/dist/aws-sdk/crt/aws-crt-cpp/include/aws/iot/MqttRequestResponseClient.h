#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/crt/Exports.h>

#include <aws/crt/Allocator.h>
#include <aws/crt/Optional.h>
#include <aws/crt/Types.h>
#include <aws/crt/Variant.h>
#include <aws/mqtt/request-response/request_response_client.h>

#include <functional>

namespace Aws
{

    namespace Crt
    {
        namespace Mqtt
        {
            class MqttConnection;
        }

        namespace Mqtt5
        {
            class Mqtt5Client;
        }
    } // namespace Crt

    namespace Iot
    {
        namespace RequestResponse
        {
            /**
             * The type of change to the state of a streaming operation subscription
             */
            enum class SubscriptionStatusEventType
            {

                /**
                 * The streaming operation is successfully subscribed to its topic (filter)
                 */
                SubscriptionEstablished = ARRSSET_SUBSCRIPTION_ESTABLISHED,

                /**
                 * The streaming operation has temporarily lost its subscription to its topic (filter)
                 */
                SubscriptionLost = ARRSSET_SUBSCRIPTION_LOST,

                /**
                 * The streaming operation has entered a terminal state where it has given up trying to subscribe
                 * to its topic (filter).  This is always due to user error (bad topic filter or IoT Core permission
                 * policy).
                 */
                SubscriptionHalted = ARRSSET_SUBSCRIPTION_HALTED,
            };

            /**
             * An event that describes a change in subscription status for a streaming operation.
             */
            class AWS_CRT_CPP_API SubscriptionStatusEvent
            {
              public:
                /**
                 * Sets the type of the event
                 *
                 * @param type kind of subscription status event this is
                 * @return reference to this
                 */
                SubscriptionStatusEvent &WithType(SubscriptionStatusEventType type)
                {
                    m_type = type;
                    return *this;
                }

                /**
                 * Sets an optional error code associated with the event
                 *
                 * @param errorCode CRT error code corresponding to the event
                 * @return reference to this
                 */
                SubscriptionStatusEvent &WithErrorCode(int errorCode)
                {
                    m_errorCode = errorCode;
                    return *this;
                }

                /**
                 * Gets the type of event
                 * @return the type of the event
                 */
                SubscriptionStatusEventType GetType() const { return m_type; }

                /**
                 * Get the error code associated with this event
                 * @return the error code associated with this event
                 */
                int GetErrorCode() const { return m_errorCode; }

              private:
                SubscriptionStatusEventType m_type = SubscriptionStatusEventType::SubscriptionEstablished;
                int m_errorCode = 0;
            };

            /**
             * Function signature of a SubscriptionStatusEvent event handler
             */
            using SubscriptionStatusEventHandler = std::function<void(SubscriptionStatusEvent &&)>;

            /**
             * An event that describes an incoming publish message received on a streaming operation.
             *
             * @internal
             */
            class AWS_CRT_CPP_API IncomingPublishEvent
            {
              public:
                /**
                 * Default constructor
                 */
                IncomingPublishEvent() : m_payload() { AWS_ZERO_STRUCT(m_payload); }

                /**
                 * Sets the message payload associated with this event.  The event does not own this payload.
                 *
                 * @param payload he message payload associated with this event
                 * @return reference to this
                 */
                IncomingPublishEvent &WithPayload(Aws::Crt::ByteCursor payload)
                {
                    m_payload = payload;
                    return *this;
                }

                /**
                 * Gets the message payload associated with this event.
                 *
                 * @return the message payload associated with this event
                 */
                Aws::Crt::ByteCursor GetPayload() const { return m_payload; }

              private:
                Aws::Crt::ByteCursor m_payload;
            };

            /**
             * Function signature of an IncomingPublishEvent event handler
             *
             * @internal
             */
            using IncomingPublishEventHandler = std::function<void(IncomingPublishEvent &&)>;

            /**
             * Encapsulates a response to an AWS IoT Core MQTT-based service request
             *
             * @internal
             */
            class AWS_CRT_CPP_API UnmodeledResponse
            {
              public:
                /**
                 * Default constructor
                 */
                UnmodeledResponse() : m_topic(), m_payload()
                {
                    AWS_ZERO_STRUCT(m_payload);
                    AWS_ZERO_STRUCT(m_topic);
                }

                /**
                 * Sets the payload of the response that correlates to a submitted request.
                 *
                 * @param payload the payload of the response that correlates to a submitted request
                 * @return reference to this
                 */
                UnmodeledResponse &WithPayload(Aws::Crt::ByteCursor payload)
                {
                    m_payload = payload;
                    return *this;
                }

                /**
                 * Sets the MQTT Topic that the response was received on.
                 *
                 * @param topic the MQTT Topic that the response was received on
                 * @return reference to this
                 */
                UnmodeledResponse &WithTopic(Aws::Crt::ByteCursor topic)
                {
                    m_topic = topic;
                    return *this;
                }

                /**
                 * Gets the payload of the response that correlates to a submitted request.
                 *
                 * @return the payload of the response that correlates to a submitted request
                 */
                Aws::Crt::ByteCursor GetPayload() const { return m_payload; }

                /**
                 * Gets the MQTT Topic that the response was received on.
                 *
                 * @return the MQTT Topic that the response was received on
                 */
                Aws::Crt::ByteCursor GetTopic() const { return m_topic; }

              private:
                /**
                 * MQTT Topic that the response was received on.  Different topics map to different types within the
                 * service model, so we need this value in order to know what to deserialize the payload into.
                 */
                Aws::Crt::ByteCursor m_topic;

                /**
                 * Payload of the response that correlates to a submitted request.
                 */
                Aws::Crt::ByteCursor m_payload;
            };

            /**
             * Either-or type that models the result of a carrying out a request - a response or an error.
             *
             * @tparam R type of a successful response
             * @tparam E type of an error
             */
            template <typename R, typename E> class Result
            {
              public:
                Result() = delete;

                explicit Result(const R &response) : m_rawResult(response) {}
                explicit Result(R &&response) : m_rawResult(std::move(response)) {}
                explicit Result(const E &error) : m_rawResult(error) {}
                explicit Result(E &&error) : m_rawResult(std::move(error)) {}

                Result &operator=(const R &response)
                {
                    this->m_rawResult = response;

                    return *this;
                }

                Result &operator=(R &&response)
                {
                    this->m_rawResult = std::move(response);

                    return *this;
                }

                Result &operator=(const E &error)
                {
                    this->m_rawResult = error;
                    return *this;
                }

                Result &operator=(E &&error)
                {
                    this->m_rawResult = std::move(error);

                    return *this;
                }

                bool IsSuccess() const { return m_rawResult.template holds_alternative<R>(); }

                const R &GetResponse() const
                {
                    AWS_FATAL_ASSERT(IsSuccess());

                    return m_rawResult.template get<R>();
                }

                const E &GetError() const
                {
                    AWS_FATAL_ASSERT(!IsSuccess());

                    return m_rawResult.template get<E>();
                }

              private:
                Aws::Crt::Variant<R, E> m_rawResult;
            };

            /**
             * Type definition for a request result where a response has not yet been deserialized into a specific
             * response type.
             */
            using UnmodeledResult = Result<UnmodeledResponse, int>;

            /**
             * Signature of a function object that handles unmodeled results.  In general, these handlers will
             * be built by service clients and are responsible for transforming an unmodeled response into a
             * modeled response.
             */
            using UnmodeledResultHandler = std::function<void(UnmodeledResult &&)>;

            /**
             * Generic configuration options for streaming operations
             *
             * @tparam T modeled message type emitted/handled by a particular stream
             */
            template <typename T> class StreamingOperationOptions
            {
              public:
                /**
                 * Sets the handler function a streaming operation will use for subscription status events.
                 *
                 * @param handler the handler function a streaming operation will use for subscription status events
                 * @return reference to this
                 */
                StreamingOperationOptions &WithSubscriptionStatusEventHandler(
                    const SubscriptionStatusEventHandler &handler)
                {
                    m_subscriptionStatusEventHandler = handler;
                    return *this;
                }

                /**
                 * Sets the handler function a streaming operation will use for the modeled message type.
                 *
                 * @param handler the handler function a streaming operation will use for the modeled message type
                 * @return reference to this
                 */
                StreamingOperationOptions &WithStreamHandler(const std::function<void(T &&)> &handler)
                {
                    m_streamHandler = handler;
                    return *this;
                }

                /**
                 * Gets the handler function a streaming operation will use for subscription status events.
                 *
                 * @return the handler function a streaming operation will use for subscription status events
                 */
                const SubscriptionStatusEventHandler &GetSubscriptionStatusEventHandler() const
                {
                    return m_subscriptionStatusEventHandler;
                }

                /**
                 * Gets the handler function a streaming operation will use for the modeled message type.
                 *
                 * @return the handler function a streaming operation will use for the modeled message type
                 */
                const std::function<void(T &&)> &GetStreamHandler() const { return m_streamHandler; }

              private:
                SubscriptionStatusEventHandler m_subscriptionStatusEventHandler;

                std::function<void(T &&)> m_streamHandler;
            };

            /**
             * Streaming operation configuration options used internally by a service client's request response
             * client.
             *
             * @internal
             */
            struct AWS_CRT_CPP_API StreamingOperationOptionsInternal
            {
              public:
                StreamingOperationOptionsInternal()
                    : subscriptionTopicFilter(), subscriptionStatusEventHandler(), incomingPublishEventHandler()
                {
                    AWS_ZERO_STRUCT(subscriptionTopicFilter);
                }

                Aws::Crt::ByteCursor subscriptionTopicFilter;

                SubscriptionStatusEventHandler subscriptionStatusEventHandler;

                IncomingPublishEventHandler incomingPublishEventHandler;
            };

            /**
             * Base type for all streaming operations
             */
            class AWS_CRT_CPP_API IStreamingOperation
            {
              public:
                /**
                 * A streaming operation is automatically closed (and an MQTT unsubscribe triggered) when its
                 * destructor is invoked.
                 */
                virtual ~IStreamingOperation() = default;

                /**
                 * Opens a streaming operation by making the appropriate MQTT subscription with the broker.
                 */
                virtual void Open() = 0;
            };

            /**
             * MQTT-based request-response client configuration options
             */
            class AWS_CRT_CPP_API RequestResponseClientOptions
            {
              public:
                /**
                 * Sets the maximum number of request-response subscriptions the client allows to be concurrently active
                 * at any one point in time.  When the client hits this threshold, requests will be delayed until
                 * earlier requests complete and release their subscriptions.  Each in-progress request will use either
                 * 1 or 2 MQTT subscriptions until completion.
                 *
                 * @param maxRequestResponseSubscriptions maximum number of concurrent subscriptions that the client
                 * will use for request-response operations
                 * @return reference to this
                 */
                RequestResponseClientOptions &WithMaxRequestResponseSubscriptions(
                    uint32_t maxRequestResponseSubscriptions)
                {
                    m_maxRequestResponseSubscriptions = maxRequestResponseSubscriptions;
                    return *this;
                }

                /**
                 * Sets the maximum number of concurrent streaming operation subscriptions that the client will allow.
                 * Each "unique" (different topic filter) streaming operation will use 1 MQTT subscription.  When the
                 * client hits this threshold, attempts to open new streaming operations will fail.
                 *
                 * @param maxStreamingSubscriptions maximum number of current subscriptions that the client will
                 * use for streaming operations
                 * @return reference to this
                 */
                RequestResponseClientOptions &WithMaxStreamingSubscriptions(uint32_t maxStreamingSubscriptions)
                {
                    m_maxStreamingSubscriptions = maxStreamingSubscriptions;
                    return *this;
                }

                /**
                 * Sets the timeout value, in seconds, for a request-response operation.  If a request is not complete
                 * by this time interval, the client will complete it as failed.  This time interval starts the instant
                 * the request is submitted to the client.
                 *
                 * @param operationTimeoutInSeconds request timeout in seconds
                 * @return reference to this
                 */
                RequestResponseClientOptions &WithOperationTimeoutInSeconds(uint32_t operationTimeoutInSeconds)
                {
                    m_operationTimeoutInSeconds = operationTimeoutInSeconds;
                    return *this;
                }

                /**
                 * Gets the maximum number of request-response subscriptions the client allows to be concurrently
                 * active.
                 *
                 * @return the maximum number of request-response subscriptions the client allows to be concurrently
                 * active
                 */
                uint32_t GetMaxRequestResponseSubscriptions() const { return m_maxRequestResponseSubscriptions; }

                /**
                 * Gets the maximum number of concurrent streaming operation subscriptions that the client will allow.
                 *
                 * @return the maximum number of concurrent streaming operation subscriptions that the client will allow
                 */
                uint32_t GetMaxStreamingSubscriptions() const { return m_maxStreamingSubscriptions; }

                /**
                 * Gets the timeout value, in seconds, for a request-response operation.
                 *
                 * @return the timeout value, in seconds, for a request-response operation
                 */
                uint32_t GetOperationTimeoutInSeconds() const { return m_operationTimeoutInSeconds; }

              private:
                /**
                 * Maximum number of subscriptions that the client will concurrently use for request-response operations
                 */
                uint32_t m_maxRequestResponseSubscriptions = 0;

                /**
                 * Maximum number of subscriptions that the client will concurrently use for streaming operations
                 */
                uint32_t m_maxStreamingSubscriptions = 0;

                /**
                 * Duration, in seconds, that a request-response operation will wait for completion before giving up
                 */
                uint32_t m_operationTimeoutInSeconds = 0;
            };

            /**
             * Generic interface for the request-response client
             */
            class AWS_CRT_CPP_API IMqttRequestResponseClient
            {
              public:
                /**
                 * There is no close operation for the client.  When the destructor is invoked, the underlying client
                 * will be closed.
                 */
                virtual ~IMqttRequestResponseClient() = default;

                /**
                 * Submits a generic request to the request-response client.
                 *
                 * @param requestOptions description of the request the client should perform
                 * @param resultHandler function object to invoke when the request is completed
                 * @return success (AWS_OP_SUCCESS) or failure (AWS_OP_ERR)
                 */
                virtual int SubmitRequest(
                    const aws_mqtt_request_operation_options &requestOptions,
                    UnmodeledResultHandler &&resultHandler) = 0;

                /**
                 * Creates a new streaming operation.  Streaming operations "listen" to a specific kind of service
                 * event and invoke handlers every time one is received.
                 *
                 * @param options configuration options for the streaming operation to construct
                 * @return
                 */
                virtual std::shared_ptr<IStreamingOperation> CreateStream(
                    const StreamingOperationOptionsInternal &options) = 0;
            };

            /**
             * Creates a new request-response client using an MQTT5 client for protocol transport
             *
             * @param protocolClient MQTT client to use for transport
             * @param options request-response client configuration options
             * @param allocator allocator to use to create the client
             * @return a new request-response client if successful, otherwise nullptr
             */
            AWS_CRT_CPP_API std::shared_ptr<IMqttRequestResponseClient> NewClientFrom5(
                const Aws::Crt::Mqtt5::Mqtt5Client &protocolClient,
                const RequestResponseClientOptions &options,
                Aws::Crt::Allocator *allocator = Aws::Crt::ApiAllocator());

            /**
             * Creates a new request-response client using an MQTT311 client for protocol transport
             *
             * @param protocolClient MQTT client to use for transport
             * @param options request-response client configuration options
             * @param allocator allocator to use to create the client
             * @return a new request-response client if successful, otherwise nullptr
             */
            AWS_CRT_CPP_API std::shared_ptr<IMqttRequestResponseClient> NewClientFrom311(
                const Aws::Crt::Mqtt::MqttConnection &protocolClient,
                const RequestResponseClientOptions &options,
                Aws::Crt::Allocator *allocator = Aws::Crt::ApiAllocator());

        } // namespace RequestResponse
    } // namespace Iot
} // namespace Aws
