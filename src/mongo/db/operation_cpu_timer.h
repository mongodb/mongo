/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;

/**
 * Implements the CPU timer for platforms that support CPU consumption tracking. Consider the
 * following when using the timer:
 *
 * All methods may only be invoked on the thread associated with the operation.
 *
 * To access the timer, the operation must be associated with a client, and the client must be
 * attached to the current thread.
 *
 * The timer is initially stopped, measures elapsed time between the invocations of `start()`
 * and `stop()`, and resets on consequent invocations of `start()`.
 *
 * To reset a timer, it should be stopped first and then started again.
 *
 * The timer is paused when the operation's client is detached from the current thread, and will
 * not resume until the client is reattached to a thread.
 */
class OperationCPUTimer {
public:
    /**
     * Returns `nullptr` if the platform does not support tracking of CPU consumption.
     */
    static OperationCPUTimer* get(OperationContext*);

    virtual Nanoseconds getElapsed() const = 0;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual void onThreadAttach() = 0;
    virtual void onThreadDetach() = 0;
};

}  // namespace mongo
