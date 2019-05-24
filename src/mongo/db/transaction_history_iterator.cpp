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

#include "mongo/platform/basic.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/logger/redaction.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * Query the oplog for an entry with the given timestamp.
 */
BSONObj findOneOplogEntry(OperationContext* opCtx, const repl::OpTime& opTime, bool permitYield) {
    BSONObj oplogBSON;
    invariant(!opTime.isNull());

    auto qr = std::make_unique<QueryRequest>(NamespaceString::kRsOplogNamespace);
    qr->setFilter(opTime.asQuery());
    qr->setOplogReplay(true);  // QueryOption_OplogReplay

    const boost::intrusive_ptr<ExpressionContext> expCtx;

    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx,
                                                     std::move(qr),
                                                     expCtx,
                                                     ExtensionsCallbackNoop(),
                                                     MatchExpressionParser::kBanAllSpecialFeatures);
    invariant(statusWithCQ.isOK(),
              str::stream() << "Failed to canonicalize oplog lookup"
                            << causedBy(statusWithCQ.getStatus()));
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    AutoGetCollectionForReadCommand ctx(opCtx,
                                        NamespaceString::kRsOplogNamespace,
                                        AutoGetCollection::ViewMode::kViewsForbidden,
                                        Date_t::max(),
                                        AutoStatsTracker::LogMode::kUpdateTop);

    auto exec =
        uassertStatusOK(getExecutorFind(opCtx, ctx.getCollection(), std::move(cq), permitYield));

    auto getNextResult = exec->getNext(&oplogBSON, nullptr);
    uassert(ErrorCodes::IncompleteTransactionHistory,
            str::stream() << "oplog no longer contains the complete write history of this "
                             "transaction, log with opTime "
                          << opTime.toBSON()
                          << " cannot be found",
            getNextResult != PlanExecutor::IS_EOF);
    if (getNextResult != PlanExecutor::ADVANCED) {
        uassertStatusOKWithContext(WorkingSetCommon::getMemberObjectStatus(oplogBSON),
                                   "PlanExecutor error in TransactionHistoryIterator");
    }

    return oplogBSON;
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
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "Missing prevTs field on oplog entry of previous write in transaction: "
                      << redact(oplogBSON),
        oplogPrevTsOption);

    _nextOpTime = oplogPrevTsOption.value();

    return oplogEntry;
}

}  // namespace mongo
