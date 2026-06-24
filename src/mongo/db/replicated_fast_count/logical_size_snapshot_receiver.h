/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/replicated_fast_count/logical_size_snapshot_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

/**
 * Receiving end for publications of the latest logical size snapshot across uncompressed
 * collections and index tables.
 */
class MONGO_MOD_OPEN LogicalSizeSnapshotReceiver {
public:
    virtual ~LogicalSizeSnapshotReceiver() = default;

    /**
     * Implementation specific logic for handling the latest logical size snapshot. Logic must be
     * non-blocking to ensure the publisher can continue making forward progress.
     */
    virtual void onLogicalSizeSnapshotPublished(const LogicalSizeSnapshot& snapshot) = 0;

    /**
     * Returns the installed receiver for this process, or nullptr if none is installed.
     */
    static LogicalSizeSnapshotReceiver* get(ServiceContext* service);
    static LogicalSizeSnapshotReceiver* get(OperationContext* opCtx);

    static void set(ServiceContext* service, std::unique_ptr<LogicalSizeSnapshotReceiver> receiver);
};

}  // namespace mongo
