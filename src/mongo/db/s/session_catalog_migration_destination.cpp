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


#include "mongo/db/s/session_catalog_migration_destination.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_retryability.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/s/session_catalog_migration.h"
#include "mongo/db/s/session_catalog_migration_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(interruptBeforeProcessingPrePostImageOriginatingOp);

const auto kOplogField = "oplog";
const WriteConcernOptions kMajorityWC(WriteConcernOptions::kMajority,
                                      WriteConcernOptions::SyncMode::UNSET,
                                      Milliseconds(0));

/**
 * Returns the command request to extract session information from the source shard.
 */
BSONObj buildMigrateSessionCmd(const MigrationSessionId& migrationSessionId) {
    BSONObjBuilder builder;
    builder.append("_getNextSessionMods", 1);
    migrationSessionId.append(&builder);
    return builder.obj();
}

/**
 * Determines whether the oplog entry has a link to either preImage/postImage and sets a new link
 * to lastResult.oplogTime. For example, if entry has link to preImageTs, this sets preImageTs to
 * lastResult.oplogTime.
 *
 * It is an error to have both preImage and postImage as well as not having them at all.
 */
void setPrePostImageTs(const SessionCatalogMigrationDestination::ProcessOplogResult& lastResult,
                       repl::MutableOplogEntry* entry) {
    if (!lastResult.isPrePostImage) {
        uassert(40628,
                str::stream() << "expected oplog with ts: " << entry->getTimestamp().toString()
                              << " to not have " << repl::OplogEntryBase::kPreImageOpTimeFieldName
                              << " or " << repl::OplogEntryBase::kPostImageOpTimeFieldName,
                !entry->getPreImageOpTime() && !entry->getPostImageOpTime());
        return;
    }

    invariant(!lastResult.oplogTime.isNull());

    uassert(40629,
            str::stream() << "expected oplog with ts: " << entry->getTimestamp().toString() << ": "
                          << redact(entry->toBSON())
                          << " to have session: " << lastResult.sessionId,
            lastResult.sessionId == entry->getSessionId());
    uassert(40630,
            str::stream() << "expected oplog with ts: " << entry->getTimestamp().toString() << ": "
                          << redact(entry->toBSON()) << " to have txnNumber: " << lastResult.txnNum,
            lastResult.txnNum == entry->getTxnNumber());

    // PM-2213 introduces oplog entries that link to pre/post images in the
    // `config.image_collection` table. For chunk migration, we downconvert to the classic format
    // where the image is stored as a no-op in the oplog. A chunk migration source will always send
    // the appropriate no-op. This code on the destination patches up the CRUD operation oplog entry
    // to look like the classic format.
    if (entry->getNeedsRetryImage()) {
        switch (entry->getNeedsRetryImage().value()) {
            case repl::RetryImageEnum::kPreImage:
                entry->setPreImageOpTime({repl::OpTime()});
                break;
            case repl::RetryImageEnum::kPostImage:
                entry->setPostImageOpTime({repl::OpTime()});
                break;
        }
        entry->setNeedsRetryImage(boost::none);
    }

    if (entry->getPreImageOpTime()) {
        entry->setPreImageOpTime(lastResult.oplogTime);
    } else if (entry->getPostImageOpTime()) {
        entry->setPostImageOpTime(lastResult.oplogTime);
    } else {
        uasserted(40631,
                  str::stream() << "expected oplog with opTime: " << entry->getOpTime().toString()
                                << ": " << redact(entry->toBSON()) << " to have either "
                                << repl::OplogEntryBase::kPreImageOpTimeFieldName << " or "
                                << repl::OplogEntryBase::kPostImageOpTimeFieldName);
    }
}

/**
 * Parses the oplog into an oplog entry and makes sure that it contains the expected fields.
 */
repl::MutableOplogEntry parseOplog(const BSONObj& oplogBSON) {
    auto oplogEntry = uassertStatusOK(repl::MutableOplogEntry::parse(oplogBSON));

    const auto& sessionInfo = oplogEntry.getOperationSessionInfo();

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have sessionId: " << redact(oplogBSON),
            sessionInfo.getSessionId());

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have txnNumber: " << redact(oplogBSON),
            sessionInfo.getTxnNumber());

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have stmtId: " << redact(oplogBSON),
            !oplogEntry.getStatementIds().empty());

    return oplogEntry;
}

/**
 * Gets the next batch of oplog entries from the source shard.
 */
BSONObj getNextSessionOplogBatch(OperationContext* opCtx,
                                 const ShardId& fromShard,
                                 const MigrationSessionId& migrationSessionId) {
    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromShard);
    uassertStatusOK(shardStatus.getStatus());

    auto shard = shardStatus.getValue();
    auto responseStatus =
        shard->runCommandWithIndefiniteRetries(opCtx,
                                               ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                               DatabaseName::kAdmin,
                                               buildMigrateSessionCmd(migrationSessionId),
                                               Shard::RetryPolicy::kNoRetry);

    uassertStatusOK(responseStatus.getStatus());
    uassertStatusOK(responseStatus.getValue().commandStatus);

    auto result = responseStatus.getValue().response;

    auto oplogElement = result[kOplogField];
    uassert(ErrorCodes::FailedToParse,
            "_getNextSessionMods response does not have the 'oplog' field as array",
            oplogElement.type() == BSONType::array);

    return result;
}
}  // namespace

SessionCatalogMigrationDestination::SessionCatalogMigrationDestination(
    NamespaceString nss,
    ShardId fromShard,
    MigrationSessionId migrationSessionId,
    CancellationToken cancellationToken)
    : _nss(std::move(nss)),
      _fromShard(std::move(fromShard)),
      _migrationSessionId(std::move(migrationSessionId)),
      _cancellationToken(std::move(cancellationToken)) {}

SessionCatalogMigrationDestination::~SessionCatalogMigrationDestination() {
    if (_thread.joinable()) {
        _errorOccurred("Destructor cleaning up thread");
        _thread.join();
    }
}

void SessionCatalogMigrationDestination::start(ServiceContext* service) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_state == State::NotStarted);
        _state = State::Migrating;
    }

    _thread = stdx::thread([=, this] {
        try {
            _retrieveSessionStateFromSource(service);
        } catch (const DBException& ex) {
            if (ex.code() == ErrorCodes::CommandNotFound) {
                // TODO: remove this after v3.7
                //
                // This means that the donor shard is running at an older version so it is safe to
                // just end this because there is no session information to transfer.
                return;
            }

            _errorOccurred(ex.toString());
        }
    });
}

void SessionCatalogMigrationDestination::finish() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_state != State::ErrorOccurred) {
        _state = State::Committing;
    }
}

bool SessionCatalogMigrationDestination::joinable() const {
    return _thread.joinable();
}

void SessionCatalogMigrationDestination::join() {
    invariant(_thread.joinable());
    _thread.join();
}

/**
 * Outline:
 *
 * 1. Get oplog with session info from the source shard.
 * 2. For each oplog entry, convert to type 'n' if not yet type 'n' while preserving all info
 *    needed for retryable writes.
 * 3. Also update the sessionCatalog for every oplog entry.
 * 4. Once the source shard returned an empty oplog buffer, it means that this should enter
 *    ReadyToCommit state and wait for the commit signal (by calling finish()).
 * 5. Once finish() is called, keep on trying to get more oplog from the source shard until it
 *    returns an empty result again.
 * 6. Wait for writes to be committed to majority of the replica set.
 */
void SessionCatalogMigrationDestination::_retrieveSessionStateFromSource(ServiceContext* service) {
    Client::initThread("sessionCatalogMigrationProducer-" + _migrationSessionId.toString(),
                       service->getService(ClusterRole::ShardServer),
                       Client::noSession());
    bool oplogDrainedAfterCommiting = false;
    ProcessOplogResult lastResult;
    repl::OpTime lastOpTimeWaited;

    while (true) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (_state == State::ErrorOccurred) {
                return;
            }
        }

        BSONObj nextBatch;
        BSONArray oplogArray;
        {
            auto executor = Grid::get(service)->getExecutorPool()->getFixedExecutor();
            auto uniqueCtx = CancelableOperationContext(
                cc().makeOperationContext(), _cancellationToken, executor);
            auto opCtx = uniqueCtx.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            nextBatch = getNextSessionOplogBatch(opCtx, _fromShard, _migrationSessionId);
            oplogArray = BSONArray{nextBatch[kOplogField].Obj()};

            if (oplogArray.isEmpty()) {
                {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    if (_state == State::Committing) {
                        // The migration is considered done only when it gets an empty result from
                        // the source shard while this is in state committing. This is to make sure
                        // that it doesn't miss any new oplog created between the time window where
                        // this depleted the buffer from the source shard and receiving the commit
                        // command.
                        if (oplogDrainedAfterCommiting) {
                            LOGV2(5087100,
                                  "Recipient finished draining oplog entries for retryable writes "
                                  "and transactions from donor again after receiving "
                                  "_recvChunkCommit",
                                  logAttrs(_nss),
                                  "migrationSessionId"_attr = _migrationSessionId,
                                  "fromShard"_attr = _fromShard);
                            break;
                        }

                        oplogDrainedAfterCommiting = true;
                    }
                }

                WriteConcernResult unusedWCResult;
                uassertStatusOK(
                    waitForWriteConcern(opCtx, lastResult.oplogTime, kMajorityWC, &unusedWCResult));

                {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    // Note: only transition to "ready to commit" if state is not error/force stop.
                    if (_state == State::Migrating) {
                        // We depleted the buffer at least once, transition to ready for commit.
                        LOGV2(5087101,
                              "Recipient finished draining oplog entries for retryable writes and "
                              "transactions from donor for the first time, before receiving "
                              "_recvChunkCommit",
                              logAttrs(_nss),
                              "migrationSessionId"_attr = _migrationSessionId,
                              "fromShard"_attr = _fromShard);
                        _state = State::ReadyToCommit;
                    }
                }

                lastOpTimeWaited = lastResult.oplogTime;
            }
        }

        for (BSONArrayIteratorSorted oplogIter(oplogArray); oplogIter.more();) {
            auto oplogEntry = oplogIter.next().Obj();
            interruptBeforeProcessingPrePostImageOriginatingOp.executeIf(
                [&](const auto&) {
                    uasserted(6749200,
                              "Intentionally failing session migration before processing post/pre "
                              "image originating update oplog entry");
                },
                [&](const auto&) {
                    return !oplogEntry["needsRetryImage"].eoo() ||
                        !oplogEntry["preImageOpTime"].eoo() || !oplogEntry["postImageOpTime"].eoo();
                });
            lastResult = _processSessionOplog(oplogEntry, lastResult, service, _cancellationToken);
        }
    }

    WriteConcernResult unusedWCResult;

    auto executor = Grid::get(service)->getExecutorPool()->getFixedExecutor();
    auto uniqueOpCtx =
        CancelableOperationContext(cc().makeOperationContext(), _cancellationToken, executor);
    uniqueOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    uassertStatusOK(
        waitForWriteConcern(uniqueOpCtx.get(), lastResult.oplogTime, kMajorityWC, &unusedWCResult));

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _state = State::Done;
    }
}

/**
 * Insert a new oplog entry by converting the oplogBSON into type 'n' oplog with the session
 * information. The new oplogEntry will also link to prePostImageTs if not null.
 */
SessionCatalogMigrationDestination::ProcessOplogResult
SessionCatalogMigrationDestination::_processSessionOplog(const BSONObj& oplogBSON,
                                                         const ProcessOplogResult& lastResult,
                                                         ServiceContext* serviceContext,
                                                         CancellationToken cancellationToken) {

    auto oplogEntry = parseOplog(oplogBSON);

    ProcessOplogResult result;
    result.sessionId = *oplogEntry.getSessionId();
    result.txnNum = *oplogEntry.getTxnNumber();

    if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
        // Note: Oplog is already no-op type, no need to nest.
        // There are three types of type 'n' oplog format expected here:
        // (1) Oplog entries that has been transformed by a previous migration into a
        //     nested oplog. In this case, o field contains {$sessionMigrateInfo: 1}
        //     and o2 field contains the details of the original oplog.
        // (2) Oplog entries that contains the pre/post-image information of a
        //     findAndModify operation. In this case, o field contains the relevant info
        //     and o2 will be empty.
        // (3) Oplog entries that are a dead sentinel, which the donor sent over as the replacement
        //     for a prepare oplog entry or unprepared transaction commit oplog entry.
        // (4) Oplog entries that are a WouldChangeOwningShard sentinel entry, used for making
        //     retries of a WouldChangeOwningShard update or findAndModify fail with
        //     IncompleteTransactionHistory. In this case, the o field is non-empty and the o2
        //     field is an empty BSONObj.

        BSONObj object2;
        if (oplogEntry.getObject2()) {
            object2 = *oplogEntry.getObject2();
        } else {
            oplogEntry.setObject2(object2);
        }

        if (object2.isEmpty() && !isWouldChangeOwningShardSentinelOplogEntry(oplogEntry)) {
            result.isPrePostImage = true;

            uassert(40632,
                    str::stream() << "Can't handle 2 pre/post image oplog in a row. Prevoius oplog "
                                  << lastResult.oplogTime.getTimestamp().toString()
                                  << ", oplog ts: " << oplogEntry.getTimestamp().toString() << ": "
                                  << oplogBSON,
                    !lastResult.isPrePostImage);
        }
    } else {
        oplogEntry.setObject2(oplogBSON);  // TODO: strip redundant info?
    }

    const auto stmtIds = oplogEntry.getStatementIds();

    auto executor = Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor();
    auto uniqueOpCtx =
        CancelableOperationContext(cc().makeOperationContext(), cancellationToken, executor);
    auto opCtx = uniqueOpCtx.get();
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Irrespective of whether or not the oplog gets logged, we want to update the
    // entriesMigrated counter to signal that we have succesfully recieved the oplog
    // from the source and have processed it.
    _sessionOplogEntriesMigrated.addAndFetch(1);

    bool beganOrContinuedTxn = false;
    bool appendedOplogEntry = false;
    while (!beganOrContinuedTxn) {
        auto conflictingTxnCompletionFuture =
            session_catalog_migration_util::runWithSessionCheckedOutIfStatementNotExecuted(
                opCtx, result.sessionId, result.txnNum, stmtIds.front(), [&] {
                    auto txnParticipant = TransactionParticipant::get(opCtx);

                    if (!result.isPrePostImage &&
                        !isWouldChangeOwningShardSentinelOplogEntry(oplogEntry)) {
                        // Do not overwrite the "o" field if this is a pre/post image oplog entry.
                        // Also do not overwrite it if this is a WouldChangeOwningShard sentinel
                        // oplog entry since it contains a special BSONObj used for making retries
                        // fail with an IncompleteTransactionHistory error.
                        oplogEntry.setObject(SessionCatalogMigration::kSessionOplogTag);
                    }
                    setPrePostImageTs(lastResult, &oplogEntry);
                    oplogEntry.setPrevWriteOpTimeInTransaction(txnParticipant.getLastWriteOpTime());

                    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
                    oplogEntry.setFromMigrate(true);
                    // Reset OpTime so logOp() can assign a new one.
                    oplogEntry.setOpTime(OplogSlot());

                    writeConflictRetry(
                        opCtx,
                        "SessionOplogMigration",
                        NamespaceString::kSessionTransactionsTableNamespace,
                        [&] {
                            // Need to take global lock here so repl::logOp will not unlock it and
                            // trigger the invariant that disallows unlocking global lock while
                            // inside a WUOW. Take the transaction table db lock to ensure the same
                            // lock ordering with normal replicated updates to the table.
                            Lock::DBLock lk(
                                opCtx,
                                NamespaceString::kSessionTransactionsTableNamespace.dbName(),
                                MODE_IX);
                            WriteUnitOfWork wunit(opCtx);

                            result.oplogTime = repl::logOp(opCtx, &oplogEntry);

                            const auto& oplogOpTime = result.oplogTime;
                            uassert(
                                40633,
                                str::stream()
                                    << "Failed to create new oplog entry for oplog with opTime: "
                                    << oplogEntry.getOpTime().toString() << ": "
                                    << redact(oplogBSON),
                                !oplogOpTime.isNull());

                            // Do not call onWriteOpCompletedO nPrimary if we inserted a pre/post
                            // image, because the next oplog will contain the real operation
                            if (!result.isPrePostImage) {
                                SessionTxnRecord sessionTxnRecord;
                                sessionTxnRecord.setSessionId(result.sessionId);
                                sessionTxnRecord.setTxnNum(result.txnNum);
                                sessionTxnRecord.setLastWriteOpTime(oplogOpTime);

                                // Use the same wallTime as oplog since SessionUpdateTracker looks
                                // at the oplog entry wallTime when replicating.
                                sessionTxnRecord.setLastWriteDate(oplogEntry.getWallClockTime());

                                if (isInternalSessionForRetryableWrite(result.sessionId)) {
                                    sessionTxnRecord.setParentSessionId(
                                        *getParentSessionId(result.sessionId));
                                }

                                // We do not migrate transaction oplog entries so don't set the txn
                                // state.
                                txnParticipant.onRetryableWriteCloningCompleted(
                                    opCtx, stmtIds, sessionTxnRecord);
                            }

                            wunit.commit();
                        });

                    appendedOplogEntry = true;
                });

        if (conflictingTxnCompletionFuture) {
            conflictingTxnCompletionFuture->wait(opCtx);
        } else {
            beganOrContinuedTxn = true;
        }
    }

    return appendedOplogEntry ? result : lastResult;
}

std::string SessionCatalogMigrationDestination::getErrMsg() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _errMsg;
}

void SessionCatalogMigrationDestination::_errorOccurred(StringData errMsg) {
    LOGV2(5087102,
          "Recipient failed to copy oplog entries for retryable writes and transactions from donor",
          logAttrs(_nss),
          "migrationSessionId"_attr = _migrationSessionId,
          "fromShard"_attr = _fromShard,
          "error"_attr = errMsg);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _state = State::ErrorOccurred;
    _errMsg = std::string{errMsg};
}

MigrationSessionId SessionCatalogMigrationDestination::getMigrationSessionId() const {
    return _migrationSessionId;
}

SessionCatalogMigrationDestination::State SessionCatalogMigrationDestination::getState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

void SessionCatalogMigrationDestination::forceFail(StringData errMsg) {
    _errorOccurred(errMsg);
}

long long SessionCatalogMigrationDestination::getSessionOplogEntriesMigrated() {
    return _sessionOplogEntriesMigrated.load();
}
}  // namespace mongo
