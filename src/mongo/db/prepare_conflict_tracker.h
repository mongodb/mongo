/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

/**
 * The PrepareConflictTracker tracks if a read operation encounters a prepare conflict. If it
 * is blocked on a prepare conflict, we will kill the operation during step down. This will
 * help us avoid deadlocks between prepare conflicts and state transitions.
 *
 * TODO SERVER-41037: Modify above comment to include step up or use "state transitions" to
 * encompass both.
 */
class PrepareConflictTracker {
public:
    static const OperationContext::Decoration<PrepareConflictTracker> get;

    /**
     * Decoration requires a default constructor.
     */
    PrepareConflictTracker() = default;

    /**
     * Returns whether a read thread is currently blocked on a prepare conflict.
     */
    bool isWaitingOnPrepareConflict() const;

    /**
     * Sets _waitOnPrepareConflict to true after a read thread hits a WT_PREPARE_CONFLICT
     * error code.
     */
    void beginPrepareConflict();

    /**
     * Sets _waitOnPrepareConflict to false after wiredTigerPrepareConflictRetry returns,
     * implying that the read thread is not blocked on a prepare conflict.
     */
    void endPrepareConflict();

private:
    /**
     * Set to true when a read operation is currently blocked on a prepare conflict.
     */
    AtomicWord<bool> _waitOnPrepareConflict{false};
};

}  // namespace mongo