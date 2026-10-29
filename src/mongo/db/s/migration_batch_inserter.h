// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ticketing/ticketholder.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/global_catalog/type_chunk.h"
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
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

// The purpose of this type is to allow inserters to communicate
// their progress to the outside world.
class MigrationCloningProgressSharedState {
    mutable std::mutex _m;
    repl::OpTime _maxOptime;
    long long _numCloned = 0;
    long long _numBytes = 0;

public:
    void updateMaxOptime(const repl::OpTime& _newOptime) {
        std::lock_guard lk(_m);
        _maxOptime = std::max(_maxOptime, _newOptime);
    }
    repl::OpTime getMaxOptime() const {
        std::lock_guard lk(_m);
        return _maxOptime;
    }
    void incNumCloned(int num) {
        std::lock_guard lk(_m);
        _numCloned += num;
    }
    void incNumBytes(int num) {
        std::lock_guard lk(_m);
        _numBytes += num;
    }
    long long getNumCloned() const {
        std::lock_guard lk(_m);
        return _numCloned;
    }
    long long getNumBytes() const {
        std::lock_guard lk(_m);
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
