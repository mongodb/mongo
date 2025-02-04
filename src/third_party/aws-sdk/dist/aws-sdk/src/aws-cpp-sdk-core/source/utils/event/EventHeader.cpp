/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/event/EventHeader.h>
#include <aws/core/utils/HashingUtils.h>

namespace Aws
{
    namespace Utils
    {
        namespace Event
        {
            static const int HASH_BOOL_TRUE = HashingUtils::HashString("BOOL_TRUE");
            static const int HASH_BOOL_FALSE = HashingUtils::HashString("BOOL_FALSE");
            static const int HASH_BYTE = HashingUtils::HashString("BYTE");
            static const int HASH_INT16 = HashingUtils::HashString("INT16");
            static const int HASH_INT32 = HashingUtils::HashString("INT32");
            static const int HASH_INT64 = HashingUtils::HashString("INT64");
            static const int HASH_BYTE_BUF = HashingUtils::HashString("BYTE_BUFFER");
            static const int HASH_STRING = HashingUtils::HashString("STRING");
            static const int HASH_TIMESTAMP = HashingUtils::HashString("TIMESTAMP");
            static const int HASH_UUID = HashingUtils::HashString("UUID");

            EventHeaderValue::EventHeaderType EventHeaderValue::GetEventHeaderTypeForName(const Aws::String& name)
            {
                int hashCode = Aws::Utils::HashingUtils::HashString(name.c_str());
                if (hashCode == HASH_BOOL_TRUE)
                {
                    return EventHeaderType::BOOL_TRUE;
                }
                else if (hashCode == HASH_BOOL_FALSE)
                {
                    return EventHeaderType::BOOL_FALSE;
                }
                else if (hashCode == HASH_BYTE)
                {
                    return EventHeaderType::BYTE;
                }
                else if (hashCode == HASH_INT16)
                {
                    return EventHeaderType::INT16;
                }
                else if (hashCode == HASH_INT32)
                {
                    return EventHeaderType::INT32;
                }
                else if (hashCode == HASH_INT64)
                {
                    return EventHeaderType::INT64;
                }
                else if (hashCode == HASH_BYTE_BUF)
                {
                    return EventHeaderType::BYTE_BUF;
                }
                else if (hashCode == HASH_STRING)
                {
                    return EventHeaderType::STRING;
                }
                else if (hashCode == HASH_TIMESTAMP)
                {
                    return EventHeaderType::TIMESTAMP;
                }
                else if (hashCode == HASH_UUID)
                {
                    return EventHeaderType::UUID;
                }
                else
                {
                    return EventHeaderType::UNKNOWN;
                }
            }

            Aws::String EventHeaderValue::GetNameForEventHeaderType(EventHeaderType value)
            {
                switch (value)
                {
                case EventHeaderType::BOOL_TRUE:
                    return "BOOL_TRUE";
                case EventHeaderType::BOOL_FALSE:
                    return "BOOL_FALSE";
                case EventHeaderType::BYTE:
                    return "BYTE";
                case EventHeaderType::INT16:
                    return "INT16";
                case EventHeaderType::INT32:
                    return "INT32";
                case EventHeaderType::INT64:
                    return "INT64";
                case EventHeaderType::BYTE_BUF:
                    return "BYTE_BUF";
                case EventHeaderType::STRING:
                    return "STRING";
                case EventHeaderType::TIMESTAMP:
                    return "TIMESTAMP";
                case EventHeaderType::UUID:
                    return "UUID";
                default:
                    return "UNKNOWN";
                }
            }

        }
    }
}

