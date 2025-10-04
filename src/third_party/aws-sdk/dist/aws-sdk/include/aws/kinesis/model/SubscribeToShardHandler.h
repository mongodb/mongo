/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/event/EventStreamHandler.h>
#include <aws/core/client/AWSError.h>
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/kinesis/KinesisErrors.h>

#include <aws/kinesis/model/SubscribeToShardInitialResponse.h>
#include <aws/kinesis/model/SubscribeToShardEvent.h>

namespace Aws
{
namespace Kinesis
{
namespace Model
{
    enum class SubscribeToShardEventType
    {
        INITIAL_RESPONSE,
        SUBSCRIBETOSHARDEVENT,
        UNKNOWN
    };

    class SubscribeToShardHandler : public Aws::Utils::Event::EventStreamHandler
    {
        typedef std::function<void(const SubscribeToShardInitialResponse&)> SubscribeToShardInitialResponseCallback;
        typedef std::function<void(const SubscribeToShardInitialResponse&, const Utils::Event::InitialResponseType)> SubscribeToShardInitialResponseCallbackEx;
        typedef std::function<void(const SubscribeToShardEvent&)> SubscribeToShardEventCallback;
        typedef std::function<void(const Aws::Client::AWSError<KinesisErrors>& error)> ErrorCallback;

    public:
        AWS_KINESIS_API SubscribeToShardHandler();
        AWS_KINESIS_API SubscribeToShardHandler& operator=(const SubscribeToShardHandler&) = default;

        AWS_KINESIS_API virtual void OnEvent() override;

        ///@{
        /**
         * Sets an initial response callback. This callback gets called on the initial SubscribeToShard Operation response.
         *   This can be either "initial-response" decoded event frame or decoded HTTP headers received on connection.
         *   This callback may get called more than once (i.e. on connection headers received and then on the initial-response event received).
         * @param callback
         */
        inline void SetInitialResponseCallbackEx(const SubscribeToShardInitialResponseCallbackEx& callback) { m_onInitialResponse = callback; }
        /**
         * Sets an initial response callback (a legacy one that does not distinguish whether response originates from headers or from the event).
         */
        inline void SetInitialResponseCallback(const SubscribeToShardInitialResponseCallback& noArgCallback)
        {
            m_onInitialResponse = [noArgCallback](const SubscribeToShardInitialResponse& rs, const Utils::Event::InitialResponseType) { return noArgCallback(rs); };
        }
        ///@}
        inline void SetSubscribeToShardEventCallback(const SubscribeToShardEventCallback& callback) { m_onSubscribeToShardEvent = callback; }
        inline void SetOnErrorCallback(const ErrorCallback& callback) { m_onError = callback; }

        inline SubscribeToShardInitialResponseCallbackEx& GetInitialResponseCallbackEx() { return m_onInitialResponse; }

    private:
        AWS_KINESIS_API void HandleEventInMessage();
        AWS_KINESIS_API void HandleErrorInMessage();
        AWS_KINESIS_API void MarshallError(const Aws::String& errorCode, const Aws::String& errorMessage);

        SubscribeToShardInitialResponseCallbackEx m_onInitialResponse;
        SubscribeToShardEventCallback m_onSubscribeToShardEvent;
        ErrorCallback m_onError;
    };

namespace SubscribeToShardEventMapper
{
    AWS_KINESIS_API SubscribeToShardEventType GetSubscribeToShardEventTypeForName(const Aws::String& name);

    AWS_KINESIS_API Aws::String GetNameForSubscribeToShardEventType(SubscribeToShardEventType value);
} // namespace SubscribeToShardEventMapper
} // namespace Model
} // namespace Kinesis
} // namespace Aws
