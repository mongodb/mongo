/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_interface_local.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/record_id.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"

#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

namespace {

class OplogIteratorLocal : public OplogInterface::Iterator {
public:
    OplogIteratorLocal(OperationContext* opCtx);

    StatusWith<Value> next() override;

private:
    CollectionAcquisition _oplogRead;
    AutoStatsTracker _tracker;
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> _exec;
};

OplogIteratorLocal::OplogIteratorLocal(OperationContext* opCtx)
    : _oplogRead(acquireCollectionMaybeLockFree(
          opCtx,
          CollectionAcquisitionRequest(NamespaceString::kRsOplogNamespace,
                                       PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                       repl::ReadConcernArgs::get(opCtx),
                                       AcquisitionPrerequisites::kRead))),
      _tracker(opCtx,
               NamespaceString::kRsOplogNamespace,
               shard_role_details::getLocker(opCtx)->isWriteLocked() ? Top::LockType::WriteLocked
                                                                     : Top::LockType::ReadLocked,
               AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
               DatabaseProfileSettings::get(opCtx->getServiceContext())
                   .getDatabaseProfileLevel(NamespaceString::kRsOplogNamespace.dbName())),
      _exec(_oplogRead.exists()
                ? InternalPlanner::collectionScan(opCtx,
                                                  _oplogRead,
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  InternalPlanner::BACKWARD)
                : nullptr) {}

StatusWith<OplogInterface::Iterator::Value> OplogIteratorLocal::next() {
    BSONObj obj;
    RecordId recordId;

    PlanExecutor::ExecState state;
    if (!_exec || PlanExecutor::ADVANCED != (state = _exec->getNext(&obj, &recordId))) {
        return StatusWith<Value>(ErrorCodes::CollectionIsEmpty,
                                 "no more operations in local oplog");
    }

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state,
              str::stream() << "Plan Executor state: " << state);

    return StatusWith<Value>(std::make_pair(obj.getOwned(), recordId));
}

}  // namespace

OplogInterfaceLocal::OplogInterfaceLocal(OperationContext* opCtx) : _opCtx(opCtx) {
    invariant(opCtx);
}

std::string OplogInterfaceLocal::toString() const {
    return str::stream() << "LocalOplogInterface: "
                            "operation context: "
                         << _opCtx->getOpID() << "; collection: "
                         << NamespaceString::kRsOplogNamespace.toStringForErrorMsg();
}

std::unique_ptr<OplogInterface::Iterator> OplogInterfaceLocal::makeIterator() const {
    return std::unique_ptr<OplogInterface::Iterator>(new OplogIteratorLocal(_opCtx));
}

std::unique_ptr<TransactionHistoryIteratorBase> OplogInterfaceLocal::makeTransactionHistoryIterator(
    const OpTime& startingOpTime, bool permitYield) const {
    return std::make_unique<TransactionHistoryIterator>(startingOpTime, permitYield);
}

HostAndPort OplogInterfaceLocal::hostAndPort() const {
    return {getHostNameCached(), serverGlobalParams.port};
}

}  // namespace repl
}  // namespace mongo
