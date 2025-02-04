/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/client/AWSError.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/event/EventStreamErrors.h>

 using namespace Aws::Client;
// using namespace Aws::S3;
// using namespace Aws::Utils;

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            namespace EventStreamErrorsMapper
            {
                /*
                static const int EVENT_STREAM_NO_ERROR_HASH = HashingUtils::HashString("EventStreamNoError");
                static const int EVENT_STREAM_BUFFER_LENGTH_MISMATCH_HASH = HashingUtils::HashString("EventStreamBufferLengthMismatch");
                static const int EVENT_STREAM_INSUFFICIENT_BUFFER_LEN_HASH = HashingUtils::HashString("EventStreamInsufficientBufferLen");
                static const int EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED_HASH = HashingUtils::HashString("EventStreamMessageFieldSizeExceeded");
                static const int EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE_HASH = HashingUtils::HashString("EventStreamPreludeChecksumFailure");
                static const int EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE_HASH = HashingUtils::HashString("EventStreamMessageChecksumFailure");
                static const int EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN_HASH = HashingUtils::HashString("EventStreamMessageInvalidHeadersLen");
                static const int EVENT_STREAM_MESSAGE_UNKNOWN_HEADER_TYPE_HASH = HashingUtils::HashString("EventStreamMessageUnknownHeaderType");
                static const int EVENT_STREAM_MESSAGE_PARSER_ILLEGAL_STATE_HASH = HashingUtils::HashString("EventStreamMessageParserIllegalState");
                */
                const char* GetNameForError(EventStreamErrors error)
                {
                    switch (error)
                    {
                    case EventStreamErrors::EVENT_STREAM_NO_ERROR:
                        return "EventStreamNoError";
                    case EventStreamErrors::EVENT_STREAM_BUFFER_LENGTH_MISMATCH:
                        return "EventStreamBufferLengthMismatch";
                    case EventStreamErrors::EVENT_STREAM_INSUFFICIENT_BUFFER_LEN:
                        return "EventStreamInsufficientBufferLen";
                    case EventStreamErrors::EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED:
                        return "EventStreamMessageFieldSizeExceeded";
                    case EventStreamErrors::EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE:
                        return "EventStreamPreludeChecksumFailure";
                    case EventStreamErrors::EVENT_STREAM_MESSAGE_CHECKSUM_FAILURE:
                        return "EventStreamMessageChecksumFailure";
                    case EventStreamErrors::EVENT_STREAM_MESSAGE_INVALID_HEADERS_LEN:
                        return "EventStreamMessageInvalidHeadersLen";
                    case EventStreamErrors::EVENT_STREAM_MESSAGE_UNKNOWN_HEADER_TYPE:
                        return "EventStreamMessageUnknownHeaderType";
                    case EventStreamErrors::EVENT_STREAM_MESSAGE_PARSER_ILLEGAL_STATE:
                        return "EventStreamMessageParserIllegalState";
                    default:
                        return "EventStreamUnknownError";
                    }
                }

                AWSError<CoreErrors> GetAwsErrorForEventStreamError(EventStreamErrors error)
                {
                    return AWSError<CoreErrors>(CoreErrors::UNKNOWN, GetNameForError(error), "", false);
                }
            } // namespace EventStreamErrorsMapper
        } // namespace Event
    } // namespace Utils
} // namespace Aws
