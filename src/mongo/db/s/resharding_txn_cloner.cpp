/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding_txn_cloner.h"

#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/redaction.h"
#include "mongo/s/shard_id.h"

namespace mongo {

using namespace fmt::literals;


std::unique_ptr<Pipeline, PipelineDeleter> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter) {
    invariant(!fetchTimestamp.isNull());

    std::list<boost::intrusive_ptr<DocumentSource>> stages;
    if (startAfter) {
        stages.emplace_back(DocumentSourceMatch::create(
            BSON(SessionTxnRecord::kSessionIdFieldName << BSON("$gt" << startAfter->toBSON())),
            expCtx));
    }
    stages.emplace_back(
        DocumentSourceSort::create(expCtx, BSON(SessionTxnRecord::kSessionIdFieldName << 1)));
    stages.emplace_back(DocumentSourceMatch::create(
        BSON((SessionTxnRecord::kLastWriteOpTimeFieldName + "." + repl::OpTime::kTimestampFieldName)
             << BSON("$lt" << fetchTimestamp)),
        expCtx));

    return Pipeline::create(std::move(stages), expCtx);
}


std::unique_ptr<Fetcher> cloneConfigTxnsForResharding(
    OperationContext* opCtx,
    const ShardId& shardId,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter,
    std::function<void(OperationContext*, BSONObj)> merge,
    Status* status) {

    boost::intrusive_ptr<ExpressionContext> expCtx = make_intrusive<ExpressionContext>(
        opCtx, nullptr, NamespaceString::kSessionTransactionsTableNamespace);
    auto pipeline =
        createConfigTxnCloningPipelineForResharding(expCtx, fetchTimestamp, std::move(startAfter));
    AggregationRequest request(NamespaceString::kSessionTransactionsTableNamespace,
                               pipeline->serializeToBson());

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kMajorityName
                                << repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                << fetchTimestamp));
    request.setHint(BSON("_id" << 1));

    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    const auto targetHost = uassertStatusOK(
        shard->getTargeter()->findHost(opCtx, ReadPreferenceSetting{ReadPreference::Nearest}));
    auto serviceContext = opCtx->getServiceContext();
    auto fetcherCallback =
        [merge, status, serviceContext](const Fetcher::QueryResponseStatus& dataStatus,
                                        Fetcher::NextAction* nextAction,
                                        BSONObjBuilder* getMoreBob) {
            if (!dataStatus.isOK()) {
                *status = dataStatus.getStatus();
                return;
            }

            ThreadClient threadClient(serviceContext);
            auto uniqueOpCtx = cc().makeOperationContext();
            auto fetcherOpCtx = uniqueOpCtx.get();
            auto data = dataStatus.getValue();
            for (BSONObj doc : data.documents) {
                try {
                    merge(fetcherOpCtx, doc);
                } catch (const DBException& ex) {
                    *status = ex.toStatus();
                    return;
                }
            }

            if (!getMoreBob) {
                return;
            }
            getMoreBob->append("getMore", data.cursorId);
            getMoreBob->append("collection", data.nss.coll());
        };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    auto fetcher = std::make_unique<Fetcher>(
        executor.get(),
        targetHost,
        "config",
        request.serializeToCommandObj().toBson(),
        fetcherCallback,
        ReadPreferenceSetting(ReadPreference::Nearest).toContainingBSON());
    uassertStatusOK(fetcher->schedule());
    return fetcher;
}

void configTxnsMergerForResharding(OperationContext* opCtx, BSONObj donorBsonTransaction) {
    SessionTxnRecord donorTransaction;

    donorTransaction = SessionTxnRecord::parse(
        IDLParserErrorContext("resharding config transactions cloning"), donorBsonTransaction);

    opCtx->setLogicalSessionId(donorTransaction.getSessionId());

    while (true) {
        auto ocs = std::make_unique<MongoDOperationContextSession>(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        // Which error code should this be? what message?
        uassert(4989900, "Failed to get transaction Participant", txnParticipant);
        try {
            txnParticipant.beginOrContinue(
                opCtx, donorTransaction.getTxnNum(), boost::none, boost::none);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::TransactionTooOld) {
                // donorTransaction.getTxnNum() < recipientTxnNumber
                return;
            } else if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
                // donorTransaction.getTxnNum() == recipientTxnNumber &&
                // !txnParticipant.transactionIsInRetryableWriteMode()
                return;
            } else if (ex.code() == ErrorCodes::PreparedTransactionInProgress) {
                // txnParticipant.transactionIsPrepared()
                ocs.reset();
                // TODO SERVER-51493 Change to not block here.
                txnParticipant.onExitPrepare().wait();
                continue;
            }
            throw;
        }

        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setObject(BSON("$sessionMigrateInfo" << 1));
        oplogEntry.setObject2(TransactionParticipant::kDeadEndSentinel);
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setSessionId(donorTransaction.getSessionId());
        oplogEntry.setTxnNumber(donorTransaction.getTxnNum());
        oplogEntry.setStatementId(kIncompleteHistoryStmtId);
        oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
        oplogEntry.setNss({});
        oplogEntry.setWallClockTime(Date_t::now());

        writeConflictRetry(
            opCtx,
            "ReshardingTxnCloner",
            NamespaceString::kSessionTransactionsTableNamespace.ns(),
            [&] {
                // Need to take global lock here so repl::logOp will not unlock it and
                // trigger the invariant that disallows unlocking global lock while
                // inside a WUOW. Take the transaction table db lock to ensure the same
                // lock ordering with normal replicated updates to the table.
                Lock::DBLock lk(
                    opCtx, NamespaceString::kSessionTransactionsTableNamespace.db(), MODE_IX);
                WriteUnitOfWork wunit(opCtx);

                repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);

                uassert(4989901,
                        str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                      << oplogEntry.getOpTime().toString() << ": "
                                      << redact(oplogEntry.toBSON()),
                        !opTime.isNull());

                SessionTxnRecord sessionTxnRecord(donorTransaction.getSessionId(),
                                                  donorTransaction.getTxnNum(),
                                                  opTime,
                                                  Date_t::now());
                txnParticipant.onRetryableWriteCloningCompleted(
                    opCtx, {kIncompleteHistoryStmtId}, sessionTxnRecord);
                wunit.commit();
            });

        return;
    }
}

}  // namespace mongo
