/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class Collection;
class Database;
class NamespaceString;
class OperationContext;
class OperationSessionInfo;
class Session;

struct OplogSlot {
    OplogSlot() {}
    OplogSlot(repl::OpTime opTime, std::int64_t hash) : opTime(opTime), hash(hash) {}
    repl::OpTime opTime;
    std::int64_t hash = 0;
};

struct InsertStatement {
public:
    InsertStatement() = default;
    explicit InsertStatement(BSONObj toInsert) : doc(toInsert) {}

    InsertStatement(StmtId statementId, BSONObj toInsert) : stmtId(statementId), doc(toInsert) {}
    InsertStatement(StmtId statementId, BSONObj toInsert, OplogSlot os)
        : stmtId(statementId), oplogSlot(os), doc(toInsert) {}
    InsertStatement(BSONObj toInsert, Timestamp ts, long long term)
        : oplogSlot(repl::OpTime(ts, term), 0), doc(toInsert) {}

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
 * Create a new capped collection for the oplog if it doesn't yet exist.
 * If the collection already exists (and isReplSet is false),
 * set the 'last' Timestamp from the last entry of the oplog collection (side effect!)
 */
void createOplog(OperationContext* opCtx, const std::string& oplogCollectionName, bool isReplSet);

/*
 * Shortcut for above function using oplogCollectionName = _oplogCollectionName,
 * and replEnabled = replCoord::isReplSet();
 */
void createOplog(OperationContext* opCtx);

extern std::string masterSlaveOplogName;

extern int OPLOG_VERSION;

/**
 * Log insert(s) to the local oplog.
 * Returns the OpTime of every insert.
 */
std::vector<OpTime> logInsertOps(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OptionalCollectionUUID uuid,
                                 Session* session,
                                 std::vector<InsertStatement>::const_iterator begin,
                                 std::vector<InsertStatement>::const_iterator end,
                                 bool fromMigrate,
                                 Date_t wallClockTime);

/**
 * @param opstr
 *  "i" insert
 *  "u" update
 *  "d" delete
 *  "c" db cmd
 *  "n" no-op
 *  "db" declares presence of a database (ns is set to the db name + '.')
 *
 * For 'u' records, 'obj' captures the mutation made to the object but not
 * the object itself. 'o2' captures the the criteria for the object that will be modified.
 *
 * wallClockTime this specifies the wall-clock timestamp of then this oplog entry was generated. It
 *   is purely informational, may not be monotonically increasing and is not interpreted in any way
 *   by the replication subsystem.
 * oplogLink this contains the timestamp that points to the previous write that will be
 *   linked via prevTs, and the timestamps of the oplog entry that contains the document
 *   before/after update was applied. The timestamps are ignored if isNull() is true.
 * oplogSlot If non-null, use this reserved oplog slot instead of a new one.
 *
 * Returns the optime of the oplog entry written to the oplog.
 * Returns a null optime if oplog was not modified.
 */
OpTime logOp(OperationContext* opCtx,
             const char* opstr,
             const NamespaceString& ns,
             OptionalCollectionUUID uuid,
             const BSONObj& obj,
             const BSONObj* o2,
             bool fromMigrate,
             Date_t wallClockTime,
             const OperationSessionInfo& sessionInfo,
             StmtId stmtId,
             const OplogLink& oplogLink,
             const OplogSlot& oplogSlot);

// Flush out the cached pointer to the oplog.
// Used by the closeDatabase command to ensure we don't cache closed things.
void oplogCheckCloseDatabase(OperationContext* opCtx, Database* db);

/**
 * Establish the cached pointer to the local oplog.
 */
void acquireOplogCollectionForLogging(OperationContext* opCtx);

using IncrementOpsAppliedStatsFn = stdx::function<void()>;
/**
 * Take the object field of a BSONObj, the BSONObj, and the namespace of
 * the operation and perform necessary validation to ensure the BSONObj is a
 * properly-formed command to insert into system.indexes. This is only to
 * be used for insert operations into system.indexes. It is called via applyOps.
 */
std::pair<BSONObj, NamespaceString> prepForApplyOpsIndexInsert(const BSONElement& fieldO,
                                                               const BSONObj& op,
                                                               const NamespaceString& requestNss);

/**
 * This class represents the different modes of oplog application that are used within the
 * replication system. Oplog application semantics may differ depending on the mode.
 *
 * It also includes functions to serialize/deserialize the oplog application mode.
 */
class OplogApplication {
public:
    static constexpr StringData kInitialSyncOplogApplicationMode = "InitialSync"_sd;
    static constexpr StringData kMasterSlaveOplogApplicationMode = "MasterSlave"_sd;
    static constexpr StringData kRecoveringOplogApplicationMode = "Recovering"_sd;
    static constexpr StringData kSecondaryOplogApplicationMode = "Secondary"_sd;
    static constexpr StringData kApplyOpsCmdOplogApplicationMode = "ApplyOps"_sd;

    enum class Mode {
        // Used during the oplog application phase of the initial sync process.
        kInitialSync,

        // Used when a slave is applying operations from a master node in master-slave.
        kMasterSlave,

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
 * Take a non-command op and apply it locally
 * Used for applying from an oplog
 * @param alwaysUpsert convert some updates to upserts for idempotency reasons
 * @param mode specifies what oplog application mode we are in
 * @param incrementOpsAppliedStats is called whenever an op is applied.
 * Returns failure status if the op was an update that could not be applied.
 */
Status applyOperation_inlock(OperationContext* opCtx,
                             Database* db,
                             const BSONObj& op,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats = {});

/**
 * Take a command op and apply it locally
 * Used for applying from an oplog
 * Returns failure status if the op that could not be applied.
 */
Status applyCommand_inlock(OperationContext* opCtx, const BSONObj& op, OplogApplication::Mode mode);

/**
 * Initializes the global Timestamp with the value from the timestamp of the last oplog entry.
 */
void initTimestampFromOplog(OperationContext* opCtx, const std::string& oplogNS);

/**
 * Sets the global Timestamp to be 'newTime'.
 */
void setNewTimestamp(ServiceContext* opCtx, const Timestamp& newTime);

/**
 * Detects the current replication mode and sets the "_oplogCollectionName" accordingly.
 */
void setOplogCollectionName();

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
                            IncrementOpsAppliedStatsFn incrementOpsAppliedStats);

/**
 * Allocates optimes for new entries in the oplog.  Returns an OplogSlot or a vector of OplogSlots,
 * which contain the new optimes along with their terms and newly calculated hash fields.
 */
OplogSlot getNextOpTime(OperationContext* opCtx);
std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count);

}  // namespace repl
}  // namespace mongo
