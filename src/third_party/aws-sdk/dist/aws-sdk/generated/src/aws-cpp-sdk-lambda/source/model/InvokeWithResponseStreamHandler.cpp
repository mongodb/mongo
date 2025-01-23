/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeWithResponseStreamHandler.h>
#include <aws/lambda/LambdaErrorMarshaller.h>
#include <aws/core/client/CoreErrors.h>
#include <aws/core/utils/event/EventStreamErrors.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Event;
using namespace Aws::Utils::Json;

AWS_CORE_API extern const char MESSAGE_LOWER_CASE[];
AWS_CORE_API extern const char MESSAGE_CAMEL_CASE[];

namespace Aws
{
namespace Lambda
{
namespace Model
{
    using namespace Aws::Client;

    static const char INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG[] = "InvokeWithResponseStreamHandler";

    InvokeWithResponseStreamHandler::InvokeWithResponseStreamHandler() : EventStreamHandler()
    {
        m_onInitialResponse = [&](const InvokeWithResponseStreamInitialResponse&, const Utils::Event::InitialResponseType eventType)
        {
            AWS_LOGSTREAM_TRACE(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG,
                "InvokeWithResponseStream initial response received from "
                << (eventType == Utils::Event::InitialResponseType::ON_EVENT ? "event" : "http headers"));
        };

        m_onInvokeResponseStreamUpdate = [&](const InvokeResponseStreamUpdate&)
        {
            AWS_LOGSTREAM_TRACE(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "InvokeResponseStreamUpdate received.");
        };

        m_onInvokeWithResponseStreamCompleteEvent = [&](const InvokeWithResponseStreamCompleteEvent&)
        {
            AWS_LOGSTREAM_TRACE(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "InvokeWithResponseStreamCompleteEvent received.");
        };

        m_onError = [&](const AWSError<LambdaErrors>& error)
        {
            AWS_LOGSTREAM_TRACE(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Lambda Errors received, " << error);
        };
    }

    void InvokeWithResponseStreamHandler::OnEvent()
    {
        // Handler internal error during event stream decoding.
        if (!*this)
        {
            AWSError<CoreErrors> error = EventStreamErrorsMapper::GetAwsErrorForEventStreamError(GetInternalError());
            error.SetMessage(GetEventPayloadAsString());
            m_onError(AWSError<LambdaErrors>(error));
            return;
        }

        const auto& headers = GetEventHeaders();
        auto messageTypeHeaderIter = headers.find(MESSAGE_TYPE_HEADER);
        if (messageTypeHeaderIter == headers.end())
        {
            AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Header: " << MESSAGE_TYPE_HEADER << " not found in the message.");
            return;
        }

        switch (Aws::Utils::Event::Message::GetMessageTypeForName(messageTypeHeaderIter->second.GetEventHeaderValueAsString()))
        {
        case Aws::Utils::Event::Message::MessageType::EVENT:
            HandleEventInMessage();
            break;
        case Aws::Utils::Event::Message::MessageType::REQUEST_LEVEL_ERROR:
        case Aws::Utils::Event::Message::MessageType::REQUEST_LEVEL_EXCEPTION:
        {
            HandleErrorInMessage();
            break;
        }
        default:
            AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG,
                "Unexpected message type: " << messageTypeHeaderIter->second.GetEventHeaderValueAsString());
            break;
        }
    }

    void InvokeWithResponseStreamHandler::HandleEventInMessage()
    {
        const auto& headers = GetEventHeaders();
        auto eventTypeHeaderIter = headers.find(EVENT_TYPE_HEADER);
        if (eventTypeHeaderIter == headers.end())
        {
            AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Header: " << EVENT_TYPE_HEADER << " not found in the message.");
            return;
        }
        switch (InvokeWithResponseStreamEventMapper::GetInvokeWithResponseStreamEventTypeForName(eventTypeHeaderIter->second.GetEventHeaderValueAsString()))
        {

        case InvokeWithResponseStreamEventType::INITIAL_RESPONSE:
        {
            InvokeWithResponseStreamInitialResponse event(GetEventHeadersAsHttpHeaders());
            m_onInitialResponse(event, Utils::Event::InitialResponseType::ON_EVENT);
            break;
        }   

        case InvokeWithResponseStreamEventType::PAYLOADCHUNK:
        {
            InvokeResponseStreamUpdate event(GetEventPayloadWithOwnership());
            m_onInvokeResponseStreamUpdate(event);
            break;
        }
        case InvokeWithResponseStreamEventType::INVOKECOMPLETE:
        {
            JsonValue json(GetEventPayloadAsString());
            if (!json.WasParseSuccessful())
            {
                AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Unable to generate a proper InvokeWithResponseStreamCompleteEvent object from the response in JSON format.");
                break;
            }

            m_onInvokeWithResponseStreamCompleteEvent(InvokeWithResponseStreamCompleteEvent{json.View()});
            break;
        }
        default:
            AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG,
                "Unexpected event type: " << eventTypeHeaderIter->second.GetEventHeaderValueAsString());
            break;
        }
    }

    void InvokeWithResponseStreamHandler::HandleErrorInMessage()
    {
        const auto& headers = GetEventHeaders();
        Aws::String errorCode;
        Aws::String errorMessage;
        auto errorHeaderIter = headers.find(ERROR_CODE_HEADER);
        if (errorHeaderIter == headers.end())
        {
            errorHeaderIter = headers.find(EXCEPTION_TYPE_HEADER);
            if (errorHeaderIter == headers.end())
            {
                AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG,
                        "Error type was not found in the event message.");
                return;
            }
        }

        errorCode = errorHeaderIter->second.GetEventHeaderValueAsString();
        errorHeaderIter = headers.find(ERROR_MESSAGE_HEADER);
        if (errorHeaderIter == headers.end())
        {
            errorHeaderIter = headers.find(EXCEPTION_TYPE_HEADER);
            if (errorHeaderIter == headers.end())
            {
                AWS_LOGSTREAM_ERROR(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG,
                        "Error description was not found in the event message.");
                return;
            }

            JsonValue exceptionPayload(GetEventPayloadAsString());
            if (!exceptionPayload.WasParseSuccessful())
            {
                AWS_LOGSTREAM_ERROR(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Unable to generate a proper InvokeWithResponseStreamCompleteEvent object from the response in JSON format.");
                auto contentTypeIter = headers.find(Aws::Utils::Event::CONTENT_TYPE_HEADER);
                if (contentTypeIter != headers.end())
                {
                    AWS_LOGSTREAM_DEBUG(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Error content-type: " << contentTypeIter->second.GetEventHeaderValueAsString());
                }
                return;
            }

            JsonView payloadView(exceptionPayload);
            errorMessage = payloadView.ValueExists(MESSAGE_CAMEL_CASE) ?
                           payloadView.GetString(MESSAGE_CAMEL_CASE) :
                           payloadView.ValueExists(MESSAGE_LOWER_CASE) ?
                           payloadView.GetString(MESSAGE_LOWER_CASE) : "";

        }
        else
        {
            errorMessage = errorHeaderIter->second.GetEventHeaderValueAsString();
        }
        MarshallError(errorCode, errorMessage);
    }

    void InvokeWithResponseStreamHandler::MarshallError(const Aws::String& errorCode, const Aws::String& errorMessage)
    {
        LambdaErrorMarshaller errorMarshaller;
        AWSError<CoreErrors> error;

        if (errorCode.empty())
        {
            error = AWSError<CoreErrors>(CoreErrors::UNKNOWN, "", errorMessage, false);
        }
        else
        {
            error = errorMarshaller.FindErrorByName(errorCode.c_str());
            if (error.GetErrorType() != CoreErrors::UNKNOWN)
            {
                AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Encountered AWSError '" << errorCode.c_str() << "': " << errorMessage.c_str());
                error.SetExceptionName(errorCode);
                error.SetMessage(errorMessage);
            }
            else
            {
                AWS_LOGSTREAM_WARN(INVOKEWITHRESPONSESTREAM_HANDLER_CLASS_TAG, "Encountered Unknown AWSError '" << errorCode.c_str() << "': " << errorMessage.c_str());
                error = AWSError<CoreErrors>(CoreErrors::UNKNOWN, errorCode, "Unable to parse ExceptionName: " + errorCode + " Message: " + errorMessage, false);
            }
        }

        m_onError(AWSError<LambdaErrors>(error));
    }

namespace InvokeWithResponseStreamEventMapper
{
    static const int INITIAL_RESPONSE_HASH = Aws::Utils::HashingUtils::HashString("initial-response");
    static const int PAYLOADCHUNK_HASH = Aws::Utils::HashingUtils::HashString("PayloadChunk");
    static const int INVOKECOMPLETE_HASH = Aws::Utils::HashingUtils::HashString("InvokeComplete");

    InvokeWithResponseStreamEventType GetInvokeWithResponseStreamEventTypeForName(const Aws::String& name)
    {
        int hashCode = Aws::Utils::HashingUtils::HashString(name.c_str());

        if (hashCode == INITIAL_RESPONSE_HASH) 
        {
            return InvokeWithResponseStreamEventType::INITIAL_RESPONSE;
        }
        else if (hashCode == PAYLOADCHUNK_HASH)
        {
            return InvokeWithResponseStreamEventType::PAYLOADCHUNK;
        }
        else if (hashCode == INVOKECOMPLETE_HASH)
        {
            return InvokeWithResponseStreamEventType::INVOKECOMPLETE;
        }
        return InvokeWithResponseStreamEventType::UNKNOWN;
    }

    Aws::String GetNameForInvokeWithResponseStreamEventType(InvokeWithResponseStreamEventType value)
    {
        switch (value)
        {
        case InvokeWithResponseStreamEventType::INITIAL_RESPONSE:
            return "initial-response";
        case InvokeWithResponseStreamEventType::PAYLOADCHUNK:
            return "PayloadChunk";
        case InvokeWithResponseStreamEventType::INVOKECOMPLETE:
            return "InvokeComplete";
        default:
            return "Unknown";
        }
    }
} // namespace InvokeWithResponseStreamEventMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
