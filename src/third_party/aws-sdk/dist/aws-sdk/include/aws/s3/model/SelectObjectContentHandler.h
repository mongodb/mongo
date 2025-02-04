/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/event/EventStreamHandler.h>
#include <aws/core/client/AWSError.h>
#include <aws/s3/S3_EXPORTS.h>
#include <aws/s3/S3Errors.h>

#include <aws/s3/model/SelectObjectContentInitialResponse.h>
#include <aws/s3/model/RecordsEvent.h>
#include <aws/s3/model/StatsEvent.h>
#include <aws/s3/model/ProgressEvent.h>

namespace Aws
{
namespace S3
{
namespace Model
{
    enum class SelectObjectContentEventType
    {
        INITIAL_RESPONSE,
        RECORDS,
        STATS,
        PROGRESS,
        CONT,
        END,
        UNKNOWN
    };

    class SelectObjectContentHandler : public Aws::Utils::Event::EventStreamHandler
    {
        typedef std::function<void(const SelectObjectContentInitialResponse&)> SelectObjectContentInitialResponseCallback;
        typedef std::function<void(const SelectObjectContentInitialResponse&, const Utils::Event::InitialResponseType)> SelectObjectContentInitialResponseCallbackEx;
        typedef std::function<void(const RecordsEvent&)> RecordsEventCallback;
        typedef std::function<void(const StatsEvent&)> StatsEventCallback;
        typedef std::function<void(const ProgressEvent&)> ProgressEventCallback;
        typedef std::function<void()> ContinuationEventCallback;
        typedef std::function<void()> EndEventCallback;
        typedef std::function<void(const Aws::Client::AWSError<S3Errors>& error)> ErrorCallback;

    public:
        AWS_S3_API SelectObjectContentHandler();
        AWS_S3_API SelectObjectContentHandler& operator=(const SelectObjectContentHandler&) = default;

        AWS_S3_API virtual void OnEvent() override;

        ///@{
        /**
         * Sets an initial response callback. This callback gets called on the initial SelectObjectContent Operation response.
         *   This can be either "initial-response" decoded event frame or decoded HTTP headers received on connection.
         *   This callback may get called more than once (i.e. on connection headers received and then on the initial-response event received).
         * @param callback
         */
        inline void SetInitialResponseCallbackEx(const SelectObjectContentInitialResponseCallbackEx& callback) { m_onInitialResponse = callback; }
        /**
         * Sets an initial response callback (a legacy one that does not distinguish whether response originates from headers or from the event).
         */
        inline void SetInitialResponseCallback(const SelectObjectContentInitialResponseCallback& noArgCallback)
        {
            m_onInitialResponse = [noArgCallback](const SelectObjectContentInitialResponse& rs, const Utils::Event::InitialResponseType) { return noArgCallback(rs); };
        }
        ///@}
        inline void SetRecordsEventCallback(const RecordsEventCallback& callback) { m_onRecordsEvent = callback; }
        inline void SetStatsEventCallback(const StatsEventCallback& callback) { m_onStatsEvent = callback; }
        inline void SetProgressEventCallback(const ProgressEventCallback& callback) { m_onProgressEvent = callback; }
        inline void SetContinuationEventCallback(const ContinuationEventCallback& callback) { m_onContinuationEvent = callback; }
        inline void SetEndEventCallback(const EndEventCallback& callback) { m_onEndEvent = callback; }
        inline void SetOnErrorCallback(const ErrorCallback& callback) { m_onError = callback; }

        inline SelectObjectContentInitialResponseCallbackEx& GetInitialResponseCallbackEx() { return m_onInitialResponse; }

    private:
        AWS_S3_API void HandleEventInMessage();
        AWS_S3_API void HandleErrorInMessage();
        AWS_S3_API void MarshallError(const Aws::String& errorCode, const Aws::String& errorMessage);

        SelectObjectContentInitialResponseCallbackEx m_onInitialResponse;
        RecordsEventCallback m_onRecordsEvent;
        StatsEventCallback m_onStatsEvent;
        ProgressEventCallback m_onProgressEvent;
        ContinuationEventCallback m_onContinuationEvent;
        EndEventCallback m_onEndEvent;
        ErrorCallback m_onError;
    };

namespace SelectObjectContentEventMapper
{
    AWS_S3_API SelectObjectContentEventType GetSelectObjectContentEventTypeForName(const Aws::String& name);

    AWS_S3_API Aws::String GetNameForSelectObjectContentEventType(SelectObjectContentEventType value);
} // namespace SelectObjectContentEventMapper
} // namespace Model
} // namespace S3
} // namespace Aws
