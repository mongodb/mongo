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

#include "mongo/db/transaction/transaction_history_iterator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/stats/top.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <utility>

namespace mongo {

namespace {

/**
 * Finds the oplog entry with the given timestamp in the oplog.
 */
BSONObj findOneOplogEntry(OperationContext* opCtx,
                          const repl::OpTime& opTime,
                          bool permitYield,
                          bool prevOpOnly = false) {
    BSONObj oplogBSON;
    invariant(!opTime.isNull());

    auto findCommand = std::make_unique<FindCommandRequest>(NamespaceString::kRsOplogNamespace);
    findCommand->setFilter(opTime.asQuery());

    if (prevOpOnly) {
        findCommand->setProjection(
            BSON("_id" << 0 << repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName << 1LL));
    }

    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build(),
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand),
                                              .allowedFeatures =
                                                  MatchExpressionParser::kBanAllSpecialFeatures},
    });
    const auto oplogRead = acquireCollectionOrViewMaybeLockFree(
        opCtx,
        CollectionOrViewAcquisitionRequest(NamespaceString::kRsOplogNamespace,
                                           PlacementConcern(boost::none, ShardVersion::UNSHARDED()),
                                           repl::ReadConcernArgs::get(opCtx),
                                           AcquisitionPrerequisites::kRead));

    const auto localDb = DatabaseHolder::get(opCtx)->getDb(opCtx, DatabaseName::kLocal);
    invariant(localDb);
    AutoStatsTracker statsTracker(opCtx,
                                  NamespaceString::kRsOplogNamespace,
                                  Top::LockType::ReadLocked,
                                  AutoStatsTracker::LogMode::kUpdateTop,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(DatabaseName::kLocal),
                                  Date_t::max());

    const auto yieldPolicy = permitYield ? PlanYieldPolicy::YieldPolicy::YIELD_AUTO
                                         : PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY;
    auto exec = uassertStatusOK(
        getExecutorFind(opCtx, MultipleCollectionAccessor{oplogRead}, std::move(cq), yieldPolicy));

    PlanExecutor::ExecState getNextResult;
    try {
        getNextResult = exec->getNext(&oplogBSON, nullptr);
    } catch (DBException& exception) {
        exception.addContext("PlanExecutor error in TransactionHistoryIterator");
        throw;
    }

    uassert(ErrorCodes::IncompleteTransactionHistory,
            str::stream() << "oplog no longer contains the complete write history of this "
                             "transaction, log with opTime "
                          << opTime.toBSON() << " cannot be found",
            getNextResult != PlanExecutor::IS_EOF);

    return oplogBSON.getOwned();
}

}  // namespace

TransactionHistoryIterator::TransactionHistoryIterator(repl::OpTime startingOpTime,
                                                       bool permitYield)
    : _permitYield(permitYield), _nextOpTime(std::move(startingOpTime)) {}

bool TransactionHistoryIterator::hasNext() const {
    return !_nextOpTime.isNull();
}

repl::OplogEntry TransactionHistoryIterator::next(OperationContext* opCtx) {
    BSONObj oplogBSON = findOneOplogEntry(opCtx, _nextOpTime, _permitYield);

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));
    const auto& oplogPrevTsOption = oplogEntry.getPrevWriteOpTimeInTransaction();
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "Missing prevOpTime field on oplog entry of previous write in transaction: "
                << redact(oplogBSON),
            oplogPrevTsOption);

    _nextOpTime = oplogPrevTsOption.value();

    return oplogEntry;
}

repl::OplogEntry TransactionHistoryIterator::nextFatalOnErrors(OperationContext* opCtx) try {
    return next(opCtx);
} catch (const DBException& ex) {
    fassertFailedWithStatus(31145, ex.toStatus());
}

repl::OpTime TransactionHistoryIterator::nextOpTime(OperationContext* opCtx) {
    BSONObj oplogBSON = findOneOplogEntry(opCtx, _nextOpTime, _permitYield, true /* prevOpOnly */);

    auto prevOpTime = oplogBSON[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName];
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "Missing prevOpTime field on oplog entry of previous write in transaction: "
                << redact(oplogBSON),
            !prevOpTime.eoo() && prevOpTime.isABSONObj());

    auto returnOpTime = _nextOpTime;
    _nextOpTime = repl::OpTime::parse(prevOpTime.Obj());
    return returnOpTime;
}

}  // namespace mongo
