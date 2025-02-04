/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/event/EventStreamHandler.h>
#include <aws/core/client/AWSError.h>
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/lambda/LambdaErrors.h>

#include <aws/lambda/model/InvokeWithResponseStreamInitialResponse.h>
#include <aws/lambda/model/InvokeResponseStreamUpdate.h>
#include <aws/lambda/model/InvokeWithResponseStreamCompleteEvent.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
    enum class InvokeWithResponseStreamEventType
    {
        INITIAL_RESPONSE,
        PAYLOADCHUNK,
        INVOKECOMPLETE,
        UNKNOWN
    };

    class InvokeWithResponseStreamHandler : public Aws::Utils::Event::EventStreamHandler
    {
        typedef std::function<void(const InvokeWithResponseStreamInitialResponse&)> InvokeWithResponseStreamInitialResponseCallback;
        typedef std::function<void(const InvokeWithResponseStreamInitialResponse&, const Utils::Event::InitialResponseType)> InvokeWithResponseStreamInitialResponseCallbackEx;
        typedef std::function<void(const InvokeResponseStreamUpdate&)> InvokeResponseStreamUpdateCallback;
        typedef std::function<void(const InvokeWithResponseStreamCompleteEvent&)> InvokeWithResponseStreamCompleteEventCallback;
        typedef std::function<void(const Aws::Client::AWSError<LambdaErrors>& error)> ErrorCallback;

    public:
        AWS_LAMBDA_API InvokeWithResponseStreamHandler();
        AWS_LAMBDA_API InvokeWithResponseStreamHandler& operator=(const InvokeWithResponseStreamHandler&) = default;

        AWS_LAMBDA_API virtual void OnEvent() override;

        ///@{
        /**
         * Sets an initial response callback. This callback gets called on the initial InvokeWithResponseStream Operation response.
         *   This can be either "initial-response" decoded event frame or decoded HTTP headers received on connection.
         *   This callback may get called more than once (i.e. on connection headers received and then on the initial-response event received).
         * @param callback
         */
        inline void SetInitialResponseCallbackEx(const InvokeWithResponseStreamInitialResponseCallbackEx& callback) { m_onInitialResponse = callback; }
        /**
         * Sets an initial response callback (a legacy one that does not distinguish whether response originates from headers or from the event).
         */
        inline void SetInitialResponseCallback(const InvokeWithResponseStreamInitialResponseCallback& noArgCallback)
        {
            m_onInitialResponse = [noArgCallback](const InvokeWithResponseStreamInitialResponse& rs, const Utils::Event::InitialResponseType) { return noArgCallback(rs); };
        }
        ///@}
        inline void SetInvokeResponseStreamUpdateCallback(const InvokeResponseStreamUpdateCallback& callback) { m_onInvokeResponseStreamUpdate = callback; }
        inline void SetInvokeWithResponseStreamCompleteEventCallback(const InvokeWithResponseStreamCompleteEventCallback& callback) { m_onInvokeWithResponseStreamCompleteEvent = callback; }
        inline void SetOnErrorCallback(const ErrorCallback& callback) { m_onError = callback; }

        inline InvokeWithResponseStreamInitialResponseCallbackEx& GetInitialResponseCallbackEx() { return m_onInitialResponse; }

    private:
        AWS_LAMBDA_API void HandleEventInMessage();
        AWS_LAMBDA_API void HandleErrorInMessage();
        AWS_LAMBDA_API void MarshallError(const Aws::String& errorCode, const Aws::String& errorMessage);

        InvokeWithResponseStreamInitialResponseCallbackEx m_onInitialResponse;
        InvokeResponseStreamUpdateCallback m_onInvokeResponseStreamUpdate;
        InvokeWithResponseStreamCompleteEventCallback m_onInvokeWithResponseStreamCompleteEvent;
        ErrorCallback m_onError;
    };

namespace InvokeWithResponseStreamEventMapper
{
    AWS_LAMBDA_API InvokeWithResponseStreamEventType GetInvokeWithResponseStreamEventTypeForName(const Aws::String& name);

    AWS_LAMBDA_API Aws::String GetNameForInvokeWithResponseStreamEventType(InvokeWithResponseStreamEventType value);
} // namespace InvokeWithResponseStreamEventMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
