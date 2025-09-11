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

#include "mongo/db/service_context.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Responsible for deleting oplog truncate markers once their max capacity has been reached.
 */
class OplogCapMaintainerThread {
public:
    static OplogCapMaintainerThread* get(ServiceContext* serviceCtx);

    /**
     * Create the maintainer thread. Must be called at most once.
     */
    void start();

    /**
     * Waits until the maintainer thread finishes. Must not be called concurrently with start().
     */
    void shutdown();

    void appendStats(BSONObjBuilder& builder) const;

private:
    void _run();

    /**
     * Returns true iff there was an oplog to delete from.
     */
    bool _deleteExcessDocuments(OperationContext* opCtx);

    stdx::thread _thread;

    // Cumulative amount of time spent truncating the oplog.
    AtomicWord<int64_t> _totalTimeTruncating;

    // Cumulative number of truncates of the oplog.
    AtomicWord<int64_t> _truncateCount;
};

}  // namespace mongo
