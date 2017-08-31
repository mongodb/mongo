/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/session_catalog.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const auto kOplogField = "oplog";
const WriteConcernOptions kMajorityWC(WriteConcernOptions::kMajority,
                                      WriteConcernOptions::SyncMode::UNSET,
                                      Milliseconds(0));

struct ProcessOplogResult {
    bool isPrePostImage = false;
    repl::OpTime oplogTime;
    LogicalSessionId sessionId;
    TxnNumber txnNum;
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
 * Determines whether the oplog entry has a link to either preImage/postImage and return
 * a new oplogLink that contains the same link, but pointing to lastResult.oplogTime. For example,
 * if entry has link to preImageTs, this returns an oplogLink with preImageTs pointing to
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
                              << repl::OplogEntryBase::kPreImageTsFieldName
                              << " or "
                              << repl::OplogEntryBase::kPostImageTsFieldName,
                !entry.getPreImageTs() && !entry.getPostImageTs());

        return oplogLink;
    }

    const auto ts = lastResult.oplogTime.getTimestamp();
    invariant(!ts.isNull());

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

    if (entry.getPreImageTs()) {
        oplogLink.preImageTs = ts;
    } else if (entry.getPostImageTs()) {
        oplogLink.postImageTs = ts;
    } else {
        uasserted(40631,
                  str::stream() << "expected oplog with ts: " << entry.getTimestamp().toString()
                                << ": "
                                << redact(entry.toBSON())
                                << " to have either "
                                << repl::OplogEntryBase::kPreImageTsFieldName
                                << " or "
                                << repl::OplogEntryBase::kPostImageTsFieldName);
    }

    return oplogLink;
}

/**
 * Parses the oplog into an oplog entry and makes sure that it contains the expected fields.
 */
repl::OplogEntry parseOplog(const BSONObj& oplogBSON) {
    auto oplogStatus = repl::OplogEntry::parse(oplogBSON);
    uassertStatusOK(oplogStatus.getStatus());

    auto oplogEntry = oplogStatus.getValue();

    auto sessionInfo = oplogEntry.getOperationSessionInfo();

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

    uassert(ErrorCodes::UnsupportedFormat,
            str::stream() << "oplog with opTime " << oplogEntry.getTimestamp().toString()
                          << " does not have o2: "
                          << redact(oplogBSON),
            oplogEntry.getObject2());

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
    auto result = responseStatus.getValue().response;

    auto oplogElement = result[kOplogField];
    uassert(ErrorCodes::FailedToParse,
            "_migrateSession response does not have the 'oplog' field as array",
            oplogElement.type() == Array);

    return result;
}

/**
 * Insert a new oplog entry by converting the oplogBSON into type 'n' oplog with the session
 * information. The new oplogEntry will also link to prePostImageTs if not null.
 */
ProcessOplogResult processSessionOplog(OperationContext* opCtx,
                                       const BSONObj& oplogBSON,
                                       // const Timestamp& prePostImageTs,
                                       const ProcessOplogResult& lastResult) {
    ProcessOplogResult result;
    auto oplogEntry = parseOplog(oplogBSON);

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

        object2 = oplogEntry.getObject2().value();

        if (object2.isEmpty()) {
            result.isPrePostImage = true;

            uassert(40632,
                    str::stream() << "Can't handle 2 pre/post image oplog in a row. Prevoius oplog "
                                  << lastResult.oplogTime.getTimestamp().toString()
                                  << ", oplog ts: "
                                  << oplogEntry.getTimestamp().toString()
                                  << ": "
                                  << redact(oplogBSON),
                    !lastResult.isPrePostImage);
        }
    } else {
        object2 = oplogBSON;  // TODO: strip redundant info?
    }

    const auto& sessionInfo = oplogEntry.getOperationSessionInfo();
    result.sessionId = sessionInfo.getSessionId().value();
    result.txnNum = sessionInfo.getTxnNumber().value();

    auto scopedSession = SessionCatalog::get(opCtx)->getOrCreateSession(opCtx, result.sessionId);
    scopedSession->beginTxn(opCtx, result.txnNum);

    BSONObj object(result.isPrePostImage
                       ? oplogEntry.getObject()
                       : BSON(SessionCatalogMigrationDestination::kSessionMigrateOplogTag << 1));
    auto oplogLink = extractPrePostImageTs(lastResult, oplogEntry);
    oplogLink.prevTs = scopedSession->getLastWriteOpTimeTs(result.txnNum);

    writeConflictRetry(opCtx,
                       "SessionOplogMigration",
                       NamespaceString::kSessionTransactionsTableNamespace.ns(),
                       [&] {
                           Lock::GlobalLock globalLock(opCtx, MODE_IX, UINT_MAX);
                           WriteUnitOfWork wunit(opCtx);

                           result.oplogTime = repl::logOp(opCtx,
                                                          "n",
                                                          oplogEntry.getNamespace(),
                                                          oplogEntry.getUuid(),
                                                          object,
                                                          &object2,
                                                          true,
                                                          sessionInfo,
                                                          *oplogEntry.getStatementId(),
                                                          oplogLink);

                           auto oplogTs = result.oplogTime.getTimestamp();
                           uassert(40633,
                                   str::stream()
                                       << "Failed to create new oplog entry for oplog with opTime: "
                                       << oplogEntry.getOpTime().toString()
                                       << ": "
                                       << redact(oplogBSON),
                                   !oplogTs.isNull());

                           if (!result.isPrePostImage) {
                               scopedSession->onWriteOpCompletedOnPrimary(
                                   opCtx, result.txnNum, {*oplogEntry.getStatementId()}, oplogTs);
                           }

                           wunit.commit();
                       });

    return result;
}
}  // unnamed namespace

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
    }

    _thread = stdx::thread(stdx::bind(
        &SessionCatalogMigrationDestination::_retrieveSessionStateFromSource, this, service));
}

void SessionCatalogMigrationDestination::finish() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _state = State::Committing;
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
        "sessionCatalogMigration-" + _migrationSessionId.toString(), service, nullptr);

    auto uniqueCtx = cc().makeOperationContext();
    auto opCtx = uniqueCtx.get();

    // Timestamp prePostImageTs;
    ProcessOplogResult lastResult;

    while (true) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (_state == State::ErrorOccurred) {
                return;
            }
        }

        try {
            auto nextBatch = getNextSessionOplogBatch(opCtx, _fromShard, _migrationSessionId);
            BSONArray oplogArray(nextBatch[kOplogField].Obj());
            BSONArrayIteratorSorted oplogIter(oplogArray);

            if (!oplogIter.more()) {
                {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    if (_state == State::Committing) {
                        break;
                    }
                }

                WriteConcernResult wcResult;
                auto wcStatus =
                    waitForWriteConcern(opCtx, lastResult.oplogTime, kMajorityWC, &wcResult);
                if (!wcStatus.isOK()) {
                    _errorOccurred(wcStatus.toString());
                    return;
                }

                // We depleted the buffer at least once, transition to ready for commit.
                {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    // Note: only transition to "ready to commit" if state is not error/force stop.
                    if (_state == State::Migrating) {
                        _state = State::ReadyToCommit;
                    }
                }
            }

            while (oplogIter.more()) {
                lastResult = processSessionOplog(opCtx, oplogIter.next().Obj(), lastResult);
            }
        } catch (const DBException& excep) {
            if (excep.code() == ErrorCodes::ConflictingOperationInProgress ||
                excep.code() == ErrorCodes::TransactionTooOld) {
                // This means that the server has a newer txnNumber than the oplog being
                // migrated, so just skip it.
                continue;
            }

            _errorOccurred(excep.toString());
            return;
        }
    }

    WriteConcernResult wcResult;
    auto wcStatus = waitForWriteConcern(opCtx, lastResult.oplogTime, kMajorityWC, &wcResult);
    if (!wcStatus.isOK()) {
        _errorOccurred(wcStatus.toString());
        return;
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _state = State::Done;
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
}

SessionCatalogMigrationDestination::State SessionCatalogMigrationDestination::getState() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

}  // namespace mongo
