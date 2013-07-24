/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string>

#include "mongo/logger/encoder.h"
#include "mongo/logger/message_event.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace logger {

    /**
     * Encoder that writes log messages of the style that MongoDB writes to console and files.
     */
    class MessageEventDetailsEncoder : public Encoder<MessageEventEphemeral> {
    public:
        typedef std::string (*DateFormatter)(Date_t);

        /**
         * Sets the date formatter function for all instances of MessageEventDetailsEncoder.
         *
         * Only and always safe to call during single-threaded execution, as in during start-up
         * intiailization.
         */
        static void setDateFormatter(DateFormatter dateFormatter);

        /**
         * Gets the date formatter function in use by instances of MessageEventDetailsEncoder.
         *
         * Always safe to call.
         */
        static DateFormatter getDateFormatter();

        virtual ~MessageEventDetailsEncoder();
        virtual std::ostream& encode(const MessageEventEphemeral& event, std::ostream& os);
    };

    /**
     * Encoder that generates log messages suitable for syslog.
     */
    class MessageEventWithContextEncoder : public Encoder<MessageEventEphemeral> {
    public:
        virtual ~MessageEventWithContextEncoder();
        virtual std::ostream& encode(const MessageEventEphemeral& event, std::ostream& os);
    };


    /**
     * Encoder that generates log messages containing only the raw text of the message.
     */
    class MessageEventUnadornedEncoder : public Encoder<MessageEventEphemeral> {
    public:
        virtual ~MessageEventUnadornedEncoder();
        virtual std::ostream& encode(const MessageEventEphemeral& event, std::ostream& os);
    };

}  // namespace logger
}  // namespace mongo
