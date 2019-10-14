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

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"

namespace mongo {
class Collection;
class Database;
class NamespaceString;
class OperationContext;
class OperationSessionInfo;
class Session;

using OplogSlot = repl::OpTime;

struct InsertStatement {
public:
    InsertStatement() = default;
    explicit InsertStatement(BSONObj toInsert) : doc(toInsert) {}

    InsertStatement(StmtId statementId, BSONObj toInsert) : stmtId(statementId), doc(toInsert) {}
    InsertStatement(StmtId statementId, BSONObj toInsert, OplogSlot os)
        : stmtId(statementId), oplogSlot(os), doc(toInsert) {}
    InsertStatement(BSONObj toInsert, Timestamp ts, long long term)
        : oplogSlot(repl::OpTime(ts, term)), doc(toInsert) {}

    StmtId stmtId = kUninitializedStmtId;
    OplogSlot oplogSlot;
    BSONObj doc;
};

namespace repl {
class ReplSettings;

struct OplogLink {
    OplogLink() = default;

    OpTime prevOpTime;
    OpTime preImageOpTime;
    OpTime postImageOpTime;
};

/**
 * Set the "lsid", "txnNumber", "stmtId", "prevOpTime", "preImageOpTime" and "postImageOpTime"
 * fields of the oplogEntry based on the given oplogLink for retryable writes (i.e. when stmtId !=
 * kUninitializedStmtId).
 *
 * If the given oplogLink.prevOpTime is a null OpTime, both the oplogLink.prevOpTime and the
 * "prevOpTime" field of the oplogEntry will be set to the TransactionParticipant's lastWriteOpTime.
 * The "preImageOpTime" field will only be set if the given oplogLink.preImageOpTime is not null.
 * Similarly, the "postImageOpTime" field will only be set if the given oplogLink.postImageOpTime is
 * not null.
 */
void appendRetryableWriteInfo(OperationContext* opCtx,
                              MutableOplogEntry* oplogEntry,
                              OplogLink* oplogLink,
                              StmtId stmtId);

/**
 * Create a new capped collection for the oplog if it doesn't yet exist.
 * If the collection already exists (and isReplSet is false),
 * set the 'last' Timestamp from the last entry of the oplog collection (side effect!)
 */
void createOplog(OperationContext* opCtx,
                 const NamespaceString& oplogCollectionName,
                 bool isReplSet);

/*
 * Shortcut for above function using oplogCollectionName = _oplogCollectionName,
 */
void createOplog(OperationContext* opCtx);

/**
 * Log insert(s) to the local oplog.
 * Returns the OpTime of every insert.
 * @param oplogEntryTemplate: a template used to generate insert oplog entries. Callers must set the
 * "ns", "ui", "fromMigrate" and "wall" fields before calling this function. This function will then
 * augment the template with the "op" (which is set to kInsert), "lsid" and "txnNumber" fields if
 * necessary.
 * @param begin/end: first/last InsertStatement to be inserted. This function iterates from begin to
 * end and generates insert oplog entries based on the augmented oplogEntryTemplate with the "ts",
 * "t", "o", "prevOpTime" and "stmtId" fields replaced by the content of each InsertStatement
 * defined by the begin-end range.
 *
 */
std::vector<OpTime> logInsertOps(OperationContext* opCtx,
                                 MutableOplogEntry* oplogEntryTemplate,
                                 std::vector<InsertStatement>::const_iterator begin,
                                 std::vector<InsertStatement>::const_iterator end);

/**
 * Returns the optime of the oplog entry written to the oplog.
 * Returns a null optime if oplog was not modified.
 */
OpTime logOp(OperationContext* opCtx, MutableOplogEntry* oplogEntry);

// Flush out the cached pointer to the oplog.
void clearLocalOplogPtr();

/**
 * Establish the cached pointer to the local oplog.
 */
void acquireOplogCollectionForLogging(OperationContext* opCtx);

/**
 * Use 'oplog' as the new cached pointer to the local oplog.
 *
 * Called by catalog::openCatalog() to re-establish the oplog collection pointer while holding onto
 * the global lock in exclusive mode.
 */
void establishOplogCollectionForLogging(OperationContext* opCtx, Collection* oplog);

using IncrementOpsAppliedStatsFn = std::function<void()>;

/**
 * This class represents the different modes of oplog application that are used within the
 * replication system. Oplog application semantics may differ depending on the mode.
 *
 * It also includes functions to serialize/deserialize the oplog application mode.
 */
class OplogApplication {
public:
    static constexpr StringData kInitialSyncOplogApplicationMode = "InitialSync"_sd;
    static constexpr StringData kRecoveringOplogApplicationMode = "Recovering"_sd;
    static constexpr StringData kSecondaryOplogApplicationMode = "Secondary"_sd;
    static constexpr StringData kApplyOpsCmdOplogApplicationMode = "ApplyOps"_sd;

    enum class Mode {
        // Used during the oplog application phase of the initial sync process.
        kInitialSync,

        // Used when we are applying oplog operations to recover the database state following an
        // unclean shutdown, or when we are recovering from the oplog after we rollback to a
        // checkpoint.
        kRecovering,

        // Used when a secondary node is applying oplog operations from the primary during steady
        // state replication.
        kSecondary,

        // Used when we are applying operations as part of a direct client invocation of the
        // 'applyOps' command.
        kApplyOpsCmd
    };

    static StringData modeToString(Mode mode);

    static StatusWith<Mode> parseMode(const std::string& mode);
};

inline std::ostream& operator<<(std::ostream& s, OplogApplication::Mode mode) {
    return (s << OplogApplication::modeToString(mode));
}

/**
 * Used for applying from an oplog entry or grouped inserts.
 * @param opOrGroupedInserts a single oplog entry or grouped inserts to be applied.
 * @param alwaysUpsert convert some updates to upserts for idempotency reasons
 * @param mode specifies what oplog application mode we are in
 * @param incrementOpsAppliedStats is called whenever an op is applied.
 * Returns failure status if the op was an update that could not be applied.
 */
Status applyOperation_inlock(OperationContext* opCtx,
                             Database* db,
                             const OplogEntryOrGroupedInserts& opOrGroupedInserts,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats = {});

/**
 * Take a command op and apply it locally
 * Used for applying from an oplog and for applyOps command.
 * Returns failure status if the op that could not be applied.
 */
Status applyCommand_inlock(OperationContext* opCtx,
                           const OplogEntry& entry,
                           OplogApplication::Mode mode);

/**
 * Initializes the global Timestamp with the value from the timestamp of the last oplog entry.
 */
void initTimestampFromOplog(OperationContext* opCtx, const NamespaceString& oplogNS);

/**
 * Sets the global Timestamp to be 'newTime'.
 */
void setNewTimestamp(ServiceContext* opCtx, const Timestamp& newTime);

/**
 * Detects the current replication mode and sets the "_oplogCollectionName" accordingly.
 */
void setOplogCollectionName(ServiceContext* service);

/**
 * Signal any waiting AwaitData queries on the oplog that there is new data or metadata available.
 */
void signalOplogWaiters();

/**
 * Creates a new index in the given namespace.
 */
void createIndexForApplyOps(OperationContext* opCtx,
                            const BSONObj& indexSpec,
                            const NamespaceString& indexNss,
                            OplogApplication::Mode mode);

/**
 * Allocates optimes for new entries in the oplog.  Returns a vector of OplogSlots, which
 * contain the new optimes along with their terms and newly calculated hash fields.
 */
std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count);

inline OplogSlot getNextOpTime(OperationContext* opCtx) {
    auto slots = getNextOpTimes(opCtx, 1);
    invariant(slots.size() == 1);
    return slots.back();
}

}  // namespace repl
}  // namespace mongo
