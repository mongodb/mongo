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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/session_catalog_migration_destination.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto kOplogField = "oplog";
const auto kWaitsForNewOplogField = "waitsForNewOplog";
const WriteConcernOptions kMajorityWC(WriteConcernOptions::kMajority,
                                      WriteConcernOptions::SyncMode::UNSET,
                                      Milliseconds(0));

struct ProcessOplogResult {
    LogicalSessionId sessionId;
    TxnNumber txnNum;

    repl::OpTime oplogTime;
    bool isPrePostImage = false;
};

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
 * Determines whether the oplog entry has a link to either preImage/postImage and return a new
 * oplogLink that contains the same link, but pointing to lastResult.oplogTime. For example, if
 * entry has link to preImageTs, this returns an oplogLink with preImageTs pointing to
 * lastResult.oplogTime.
 *
 * It is an error to have both preImage and postImage as well as not having them at all.
 */
repl::OplogLink extractPrePostImageTs(const ProcessOplogResult& lastResult,
                                      const repl::OplogEntry& entry) {
    repl::OplogLink oplogLink;

    if (!lastResult.isPrePostImage) {
        uassert(40628,
                str::stream() << "expected oplog with ts: " << entry.getTimestamp().toString()
                              << " to not have "
                              << repl::OplogEntryBase::kPreImageOpTimeFieldName
                              << " or "
                              << repl::OplogEntryBase::kPostImageOpTimeFieldName,
                !entry.getPreImageOpTime() && !entry.getPostImageOpTime());

        return oplogLink;
    }

    invariant(!lastResult.oplogTime.isNull());

    const auto& sessionInfo = entry.getOperationSessionInfo();
    const auto sessionId = *sessionInfo.getSessionId();
    const auto txnNum = *sessionInfo.getTxnNumber();

    uassert(40629,
            str::stream() << "expected oplog with ts: " << entry.getTimestamp().toString() << ": "
                          << redact(entry.toBSON())
                          << " to have session: "
                          << lastResult.sessionId,
            lastResult.sessionId == sessionId);
    uassert(40630,
            str::stream() << "expected oplog with ts: " << entry.getTimestamp().toString() << ": "
                          << redact(entry.toBSON())
                          << " to have txnNumber: "
                          << lastResult.txnNum,
            lastResult.txnNum == txnNum);

    if (entry.getPreImageOpTime()) {
        oplogLink.preImageOpTime = lastResult.oplogTime;
    } else if (entry.getPostImageOpTime()) {
        oplogLink.postImageOpTime = lastResult.oplogTime;
    } else {
        uasserted(40631,
                  str::stream() << "expected oplog with opTime: " << entry.getOpTime().toString()
                                << ": "
                                << redact(entry.toBSON())
                                << " to have either "
                                << repl::OplogEntryBase::kPreImageOpTimeFieldName
                                << " or "
                                << repl::OplogEntryBase::kPostImageOpTimeFieldName);
    }

    return oplogLink;
}

/**
 * Parses the oplog into an oplog entry and makes sure that it contains the expected fields.
 */
repl::OplogEntry parseOplog(const BSONObj& oplogBSON) {
    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogBSON));

    // Session oplog entries must always contain wall clock time, because we will not be
    // transferring anything from a previous version of the server
    invariant(oplogEntry.getWallClockTime());

    const auto& sessionInfo = oplogEntry.getOperationSessionInfo();

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have sessionId: "
                          << redact(oplogBSON),
            sessionInfo.getSessionId());

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have txnNumber: "
                          << redact(oplogBSON),
            sessionInfo.getTxnNumber());

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have stmtId: "
                          << redact(oplogBSON),
            oplogEntry.getStatementId());

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
    auto responseStatus = shard->runCommand(opCtx,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            "admin",
                                            buildMigrateSessionCmd(migrationSessionId),
                                            Shard::RetryPolicy::kNoRetry);

    uassertStatusOK(responseStatus.getStatus());
    uassertStatusOK(responseStatus.getValue().commandStatus);

    auto result = responseStatus.getValue().response;

    auto oplogElement = result[kOplogField];
    uassert(ErrorCodes::FailedToParse,
            "_getNextSessionMods response does not have the 'oplog' field as array",
            oplogElement.type() == Array);

    return result;
}

/**
 * Insert a new oplog entry by converting the oplogBSON into type 'n' oplog with the session
 * information. The new oplogEntry will also link to prePostImageTs if not null.
 */
ProcessOplogResult processSessionOplog(const BSONObj& oplogBSON,
                                       const ProcessOplogResult& lastResult) {
    auto oplogEntry = parseOplog(oplogBSON);
    const auto& sessionInfo = oplogEntry.getOperationSessionInfo();

    ProcessOplogResult result;
    result.sessionId = *sessionInfo.getSessionId();
    result.txnNum = *sessionInfo.getTxnNumber();

    BSONObj object2;
    if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
        // Note: Oplog is already no-op type, no need to nest.
        // There are two types of type 'n' oplog format expected here:
        // (1) Oplog entries that has been transformed by a previous migration into a
        //     nested oplog. In this case, o field contains {$sessionMigrateInfo: 1}
        //     and o2 field contains the details of the original oplog.
        // (2) Oplog entries that contains the pre/post-image information of a
        //     findAndModify operation. In this case, o field contains the relevant info
        //     and o2 will be empty.

        if (oplogEntry.getObject2()) {
            object2 = *oplogEntry.getObject2();
        }

        if (object2.isEmpty()) {
            result.isPrePostImage = true;

            uassert(40632,
                    str::stream() << "Can't handle 2 pre/post image oplog in a row. Prevoius oplog "
                                  << lastResult.oplogTime.getTimestamp().toString()
                                  << ", oplog ts: "
                                  << oplogEntry.getTimestamp().toString()
                                  << ": "
                                  << oplogBSON,
                    !lastResult.isPrePostImage);
        }
    } else {
        object2 = oplogBSON;  // TODO: strip redundant info?
    }

    const auto stmtId = *oplogEntry.getStatementId();

    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();
    opCtx->setLogicalSessionId(result.sessionId);
    opCtx->setTxnNumber(result.txnNum);
    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        txnParticipant.beginOrContinue(opCtx, result.txnNum, boost::none, boost::none);
        if (txnParticipant.checkStatementExecuted(opCtx, stmtId)) {
            // Skip the incoming statement because it has already been logged locally
            return lastResult;
        }
    } catch (const DBException& ex) {
        // If the transaction chain is incomplete because oplog was truncated, just ignore the
        // incoming oplog and don't attempt to 'patch up' the missing pieces.
        if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            return lastResult;
        }

        if (stmtId == kIncompleteHistoryStmtId) {
            // No need to log entries for transactions whose history has been truncated
            return lastResult;
        }

        throw;
    }

    BSONObj object(result.isPrePostImage
                       ? oplogEntry.getObject()
                       : BSON(SessionCatalogMigrationDestination::kSessionMigrateOplogTag << 1));
    auto oplogLink = extractPrePostImageTs(lastResult, oplogEntry);
    oplogLink.prevOpTime = txnParticipant.getLastWriteOpTime();

    writeConflictRetry(
        opCtx,
        "SessionOplogMigration",
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        [&] {
            // Need to take global lock here so repl::logOp will not unlock it and trigger the
            // invariant that disallows unlocking global lock while inside a WUOW. Grab a DBLock
            // here instead of plain GlobalLock to make sure the MMAPV1 flush lock will be
            // lock/unlocked correctly. Take the transaction table db lock to ensure the same lock
            // ordering with normal replicated updates to the table.
            Lock::DBLock lk(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db(), MODE_IX);
            WriteUnitOfWork wunit(opCtx);

            result.oplogTime = repl::logOp(opCtx,
                                           "n",
                                           oplogEntry.getNss(),
                                           oplogEntry.getUuid(),
                                           object,
                                           &object2,
                                           true,
                                           *oplogEntry.getWallClockTime(),
                                           sessionInfo,
                                           stmtId,
                                           oplogLink,
                                           OplogSlot());

            const auto& oplogOpTime = result.oplogTime;
            uassert(40633,
                    str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                  << oplogEntry.getOpTime().toString()
                                  << ": "
                                  << redact(oplogBSON),
                    !oplogOpTime.isNull());

            // Do not call onWriteOpCompletedOnPrimary if we inserted a pre/post image, because the
            // next oplog will contain the real operation
            if (!result.isPrePostImage) {
                txnParticipant.onMigrateCompletedOnPrimary(
                    opCtx, result.txnNum, {stmtId}, oplogOpTime, *oplogEntry.getWallClockTime());
            }

            wunit.commit();
        });

    return result;
}

}  // namespace

const char SessionCatalogMigrationDestination::kSessionMigrateOplogTag[] = "$sessionMigrateInfo";

SessionCatalogMigrationDestination::SessionCatalogMigrationDestination(
    ShardId fromShard, MigrationSessionId migrationSessionId)
    : _fromShard(std::move(fromShard)), _migrationSessionId(std::move(migrationSessionId)) {}

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
        _isStateChanged.notify_all();
    }

    _thread = stdx::thread([=] {
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
        _isStateChanged.notify_all();
    }
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
    Client::initThread(
        "sessionCatalogMigrationProducer-" + _migrationSessionId.toString(), service, nullptr);

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
            auto uniqueCtx = cc().makeOperationContext();
            auto opCtx = uniqueCtx.get();

            nextBatch = getNextSessionOplogBatch(opCtx, _fromShard, _migrationSessionId);
            oplogArray = BSONArray{nextBatch[kOplogField].Obj()};
            const auto donorWaitsForNewOplog = nextBatch[kWaitsForNewOplogField].trueValue();

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
                            break;
                        }

                        oplogDrainedAfterCommiting = true;
                    }
                }

                WriteConcernResult unusedWCResult;
                uassertStatusOK(
                    waitForWriteConcern(opCtx, lastResult.oplogTime, kMajorityWC, &unusedWCResult));

                // We depleted the buffer at least once, transition to ready for commit.
                {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    // Note: only transition to "ready to commit" if state is not error/force stop.
                    if (_state == State::Migrating) {
                        _state = State::ReadyToCommit;
                        _isStateChanged.notify_all();
                    }
                }

                // TODO: SERVER-40187 Completely remove after v4.2. donorWaitsForNewOplog is a
                // 'feature flag' indicating that the donor does block until there are new oplog
                // to return so we don't need to sleep here.
                if (!donorWaitsForNewOplog && lastOpTimeWaited == lastResult.oplogTime) {
                    // We got an empty result at least twice in a row from the source shard so space
                    // it out a little bit so we don't hammer the shard
                    opCtx->sleepFor(Milliseconds(200));
                }

                lastOpTimeWaited = lastResult.oplogTime;
            }
        }
        for (BSONArrayIteratorSorted oplogIter(oplogArray); oplogIter.more();) {
            try {
                lastResult = processSessionOplog(oplogIter.next().Obj(), lastResult);
            } catch (const ExceptionFor<ErrorCodes::ConfigurationInProgress>&) {
                // This means that the server has a newer txnNumber than the oplog being
                // migrated, so just skip it
                continue;
            } catch (const ExceptionFor<ErrorCodes::TransactionTooOld>&) {
                // This means that the server has a newer txnNumber than the oplog being
                // migrated, so just skip it
                continue;
            }
        }
    }

    WriteConcernResult unusedWCResult;
    auto uniqueOpCtx = cc().makeOperationContext();
    uassertStatusOK(
        waitForWriteConcern(uniqueOpCtx.get(), lastResult.oplogTime, kMajorityWC, &unusedWCResult));

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _state = State::Done;
        _isStateChanged.notify_all();
    }
}

std::string SessionCatalogMigrationDestination::getErrMsg() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _errMsg;
}

void SessionCatalogMigrationDestination::_errorOccurred(StringData errMsg) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _state = State::ErrorOccurred;
    _errMsg = errMsg.toString();

    _isStateChanged.notify_all();
}

SessionCatalogMigrationDestination::State SessionCatalogMigrationDestination::getState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

void SessionCatalogMigrationDestination::forceFail(StringData errMsg) {
    _errorOccurred(errMsg);
}

}  // namespace mongo
