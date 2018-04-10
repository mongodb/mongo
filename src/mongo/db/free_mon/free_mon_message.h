/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <condition_variable>
#include <vector>

#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Message types for free monitoring.
 *
 * Some are generated internally by FreeMonProcessor to handle async HTTP requests.
 */
enum class FreeMonMessageType {
    /**
     * Register server from command-line/config.
     */
    RegisterServer,

    /**
     * Register server from server command.
     */
    RegisterCommand,

    /**
     * Internal: Generated when an async HTTP request completes succesfully.
     */
    AsyncRegisterComplete,

    /**
     * Internal: Generated when an async HTTP request completes with an error.
     */
    AsyncRegisterFail,

    // TODO - add metrics messages
    // MetricsCollect - Cloud wants the "wait" time to calculated when the message processing
    // starts, not ends
    // AsyncMetricsComplete,
    // AsyncMetricsFail,

    // TODO - add replication messages
    // OnPrimary,
    // OpObserver,
};

/**
 * Message class that encapsulate a message to the FreeMonMessageProcessor
 *
 * Has a type and a deadline for when to start processing the message.
 */
class FreeMonMessage {
public:
    virtual ~FreeMonMessage();

    /**
     * Create a message that should processed immediately.
     */
    static std::shared_ptr<FreeMonMessage> createNow(FreeMonMessageType type) {
        return std::make_shared<FreeMonMessage>(type, Date_t::min());
    }

    /**
     * Create a message that should processed after the specified deadline.
     */
    static std::shared_ptr<FreeMonMessage> createWithDeadline(FreeMonMessageType type,
                                                              Date_t deadline) {
        return std::make_shared<FreeMonMessage>(type, deadline);
    }

    FreeMonMessage(const FreeMonMessage&) = delete;
    FreeMonMessage(FreeMonMessage&&) = default;

    /**
     * Get the type of message.
     */
    FreeMonMessageType getType() const {
        return _type;
    }

    /**
     * Get the deadline for the message.
     */
    Date_t getDeadline() const {
        return _deadline;
    }

public:
    FreeMonMessage(FreeMonMessageType type, Date_t deadline) : _type(type), _deadline(deadline) {}

private:
    // Type of message
    FreeMonMessageType _type;

    // Deadline for when to process message
    Date_t _deadline;
};


}  // namespace mongo
