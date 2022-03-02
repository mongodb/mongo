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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/batched_delete_stage.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/pm2423_feature_flags_gen.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

BatchedDeleteStage::BatchedDeleteStage(ExpressionContext* expCtx,
                                       std::unique_ptr<DeleteStageParams> params,
                                       std::unique_ptr<BatchedDeleteStageBatchParams> batchParams,
                                       WorkingSet* ws,
                                       const CollectionPtr& collection,
                                       PlanStage* child)
    : DeleteStage::DeleteStage(
          kStageType.rawData(), expCtx, std::move(params), ws, collection, child),
      _batchParams(std::move(batchParams)) {
    uassert(6303800,
            "batched deletions only support multi-document deletions (multi: true)",
            _params->isMulti);
    tassert(6303801,
            "batched deletions do not support the 'fromMigrate' parameter",
            !_params->fromMigrate);
    tassert(6303802,
            "batched deletions do not support the 'returnDelete' parameter",
            !_params->returnDeleted);
    tassert(
        6303803, "batched deletions do not support the 'sort' parameter", _params->sort.isEmpty());
    tassert(6303804,
            "batched deletions do not support the 'removeSaver' parameter",
            _params->sort.isEmpty());
    tassert(6303805,
            "batched deletions do not support the 'numStatsForDoc' parameter",
            !_params->numStatsForDoc);
    tassert(6303806,
            "batch size cannot be unbounded; you must specify at least one of the following batch "
            "parameters: "
            "'targetBatchBytes', 'targetBatchDocs', 'targetBatchTimeMS'",
            _batchParams->targetBatchBytes || _batchParams->targetBatchDocs ||
                _batchParams->targetBatchTimeMS != Milliseconds(0));
}

BatchedDeleteStage::~BatchedDeleteStage() {}

PlanStage::StageState BatchedDeleteStage::_deleteBatch(WorkingSetID* out) {
    try {
        child()->saveState();
    } catch (const WriteConflictException&) {
        std::terminate();
    }

    // TODO (SERVER-63047): use a single write timestamp by grouping oplog entries.
    opCtx()->recoveryUnit()->ignoreAllMultiTimestampConstraints();

    unsigned int docsDeleted = 0;

    try {
        WriteUnitOfWork wuow(opCtx());

        while (!_ridBuffer.empty()) {
            const auto rid = _ridBuffer.front();
            _ridBuffer.pop_front();

            // TODO (SERVER-63863): skip the record if it does not exist any longer.

            collection()->deleteDocument(opCtx(),
                                         _params->stmtId,
                                         rid,
                                         _params->opDebug,
                                         _params->fromMigrate,
                                         false,
                                         _params->returnDeleted ? Collection::StoreDeletedDoc::On
                                                                : Collection::StoreDeletedDoc::Off);
            docsDeleted++;
        }

        wuow.commit();
    } catch (const WriteConflictException&) {
        // TODO (SERVER-63863): retriability on write conflict exception.
    }

    _specificStats.docsDeleted += docsDeleted;

    try {
        child()->restoreState(&collection());
    } catch (const WriteConflictException&) {
        // Note we don't need to retry anything in this case since the delete already was committed.
        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
    }

    return NEED_TIME;
}

PlanStage::StageState BatchedDeleteStage::doWork(WorkingSetID* out) {
    // TODO (SERVER-63863): handle retries due to a previous write conflict exception.
    WorkingSetID id;
    auto status = child()->work(&id);

    switch (status) {
        case PlanStage::ADVANCED:
            break;

        case PlanStage::NEED_TIME:
            return status;

        case PlanStage::NEED_YIELD:
            *out = id;
            return status;

        case PlanStage::IS_EOF:
            if (!_ridBuffer.empty()) {
                // Drain the outstanding deletions.
                auto ret = _deleteBatch(out);
                if (ret != NEED_TIME) {
                    return ret;
                }
            }
            return status;

        default:
            MONGO_UNREACHABLE;
    }

    // We advanced, or are retrying, and id is set to the WSM to work on.
    WorkingSetMember* member = _ws->get(id);

    // We want to free this member when we return, unless we need to retry deleting it.
    ScopeGuard memberFreer([&] { _ws->free(id); });

    invariant(member->hasRecordId());
    RecordId recordId = member->recordId;
    // Deletes can't have projections. This means that covering analysis will always add
    // a fetch. We should always get fetched data, and never just key data.
    invariant(member->hasObj());

    // Ensure that the BSONObj underlying the WSM is owned because saveState() is
    // allowed to free the memory the BSONObj points to. The BSONObj will be needed
    // later when it is passed to Collection::deleteDocument(). Note that the call to
    // makeObjOwnedIfNeeded() will leave the WSM in the RID_AND_OBJ state in case we need to retry
    // deleting it.
    member->makeObjOwnedIfNeeded();

    // Do the write, unless this is an explain.
    if (!_params->isExplain) {
        _ridBuffer.emplace_back(recordId);
        if (_batchParams->targetBatchDocs && _ridBuffer.size() >= _batchParams->targetBatchDocs) {
            return _deleteBatch(out);
        }
    }

    return PlanStage::NEED_TIME;
}

}  // namespace mongo
