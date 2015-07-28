/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <iosfwd>

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
    typedef void (*DateFormatter)(std::ostream&, Date_t);

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
