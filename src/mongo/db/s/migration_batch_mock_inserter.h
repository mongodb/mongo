/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/migration_batch_inserter.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"

#pragma once

namespace mongo {

class MigrationBatchMockInserter {
public:
    void run(Status status) const {
        // Run is passed in a non-ok status if this function runs inline.
        // That happens if we schedule this task on a ThreadPool that is
        // already shutdown.  We should never do that.  Therefore,
        // we assert that here.
        invariant(status);
    }
    MigrationBatchMockInserter(OperationContext*,
                               OperationContext*,
                               BSONObj,
                               NamespaceString,
                               ChunkRange,
                               WriteConcernOptions,
                               UUID,
                               std::shared_ptr<MigrationCloningProgressSharedState>,
                               UUID,
                               TicketHolder*) {}

    static void onCreateThread(const std::string& threadName) {}

private:
    BSONObj _batch;
};

}  // namespace mongo
