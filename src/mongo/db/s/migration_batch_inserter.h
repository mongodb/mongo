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

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_exec.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/range_deletion_util.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

// The purpose of this type is to allow inserters to communicate
// their progress to the outside world.
class MigrationCloningProgressSharedState {
    mutable stdx::mutex _m;
    repl::OpTime _maxOptime;
    long long _numCloned = 0;
    long long _numBytes = 0;

public:
    void updateMaxOptime(const repl::OpTime& _newOptime) {
        stdx::lock_guard lk(_m);
        _maxOptime = std::max(_maxOptime, _newOptime);
    }
    repl::OpTime getMaxOptime() const {
        stdx::lock_guard lk(_m);
        return _maxOptime;
    }
    void incNumCloned(int num) {
        stdx::lock_guard lk(_m);
        _numCloned += num;
    }
    void incNumBytes(int num) {
        stdx::lock_guard lk(_m);
        _numBytes += num;
    }
    long long getNumCloned() const {
        stdx::lock_guard lk(_m);
        return _numCloned;
    }
    long long getNumBytes() const {
        stdx::lock_guard lk(_m);
        return _numBytes;
    }
};

// This type contains a BSONObj _batch corresponding to a _migrateClone response.
// The purpose of this type is to perform the insertions for this batch.
// Those insertions happen in its "run" method.  The MigrationBatchFetcher
// schedules these jobs on a thread pool.  This class has no knowledge that it runs
// on a thread pool.  It sole purpose is to perform insertions and communicate its progress
// (inluding the  new max opTime).
class MigrationBatchInserter {
public:
    // Do inserts.
    void run(Status status) const;

    MigrationBatchInserter(OperationContext* outerOpCtx,
                           OperationContext* innerOpCtx,
                           BSONObj batch,
                           const NamespaceString& nss,
                           const ChunkRange& range,
                           const WriteConcernOptions& writeConcern,
                           const UUID& collectionUuid,
                           std::shared_ptr<MigrationCloningProgressSharedState> migrationProgress,
                           const UUID& migrationId,
                           TicketHolder* secondaryThrottleTicket)
        : _outerOpCtx{outerOpCtx},
          _innerOpCtx{innerOpCtx},
          _batch{batch},
          _nss{nss},
          _range{range},
          _writeConcern{writeConcern},
          _collectionUuid{collectionUuid},
          _migrationProgress{migrationProgress},
          _migrationId{migrationId},
          _secondaryThrottleTicket{secondaryThrottleTicket} {}

    static void onCreateThread(const std::string& threadName);

private:
    OperationContext* _outerOpCtx;
    OperationContext* _innerOpCtx;
    BSONObj _batch;
    NamespaceString _nss;
    ChunkRange _range;
    WriteConcernOptions _writeConcern;
    UUID _collectionUuid;
    std::shared_ptr<MigrationCloningProgressSharedState> _migrationProgress;
    UUID _migrationId;
    TicketHolder* _secondaryThrottleTicket;
};

}  // namespace mongo
