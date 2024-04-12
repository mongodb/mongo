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

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_constraint_violation_logger.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_or_grouped_inserts.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
class Collection;
class CollectionPtr;
class Database;
class NamespaceString;
class OperationContext;
class OperationSessionInfo;
class CollectionAcquisition;
class Session;

using OplogSlot = repl::OpTime;

struct InsertStatement {
public:
    InsertStatement() = default;
    explicit InsertStatement(BSONObj toInsert) : doc(std::move(toInsert)) {}

    InsertStatement(std::vector<StmtId> statementIds, BSONObj toInsert)
        : stmtIds(statementIds), doc(std::move(toInsert)) {}
    InsertStatement(StmtId stmtId, BSONObj toInsert)
        : InsertStatement(std::vector<StmtId>{stmtId}, std::move(toInsert)) {}

    InsertStatement(std::vector<StmtId> statementIds, BSONObj toInsert, OplogSlot os)
        : stmtIds(statementIds), oplogSlot(std::move(os)), doc(std::move(toInsert)) {}
    InsertStatement(StmtId stmtId, BSONObj toInsert, OplogSlot os)
        : InsertStatement(std::vector<StmtId>{stmtId}, std::move(toInsert), std::move(os)) {}

    InsertStatement(BSONObj toInsert, Timestamp ts, long long term)
        : oplogSlot(repl::OpTime(ts, term)), doc(std::move(toInsert)) {}

    InsertStatement(BSONObj toInsert, RecordId rid)
        : recordId(std::move(rid)), doc(std::move(toInsert)) {}

    std::vector<StmtId> stmtIds = {kUninitializedStmtId};
    OplogSlot oplogSlot;

    // TODO SERVER-86241: Clarify whether this is just used for testing and whether it is necessary
    // at all. When a collection has replicated record ids enabled, defer to the
    // 'replicatedRecordId' as the source of truth.
    //
    // Caution: this may be an artifact of code movement, and its current purpose is unclear.
    RecordId recordId;

    // Holds the replicated recordId during secondary oplog application.
    RecordId replicatedRecordId;

    BSONObj doc;
};

namespace repl {
class ReplSettings;

struct OplogLink {
    OplogLink() = default;

    OpTime prevOpTime;
    MultiOplogEntryType multiOpType = MultiOplogEntryType::kLegacyMultiOpType;
};

/**
 * Set the "lsid", "txnNumber", "stmtId", "prevOpTime" fields of the oplogEntry based on the given
 * oplogLink for retryable writes (i.e. when stmtIds.front() != kUninitializedStmtId).
 *
 * If the given oplogLink.prevOpTime is a null OpTime, both the oplogLink.prevOpTime and the
 * "prevOpTime" field of the oplogEntry will be set to the TransactionParticipant's lastWriteOpTime.
 */
void appendOplogEntryChainInfo(OperationContext* opCtx,
                               MutableOplogEntry* oplogEntry,
                               OplogLink* oplogLink,
                               const std::vector<StmtId>& stmtIds);

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
 * Returns the optime of the oplog entry written to the oplog.
 * Returns a null optime if oplog was not modified.
 */
OpTime logOp(OperationContext* opCtx, MutableOplogEntry* oplogEntry);

/**
 * Low level oplog function used by logOp() and similar functions to append
 * storage engine records to the oplog collection.
 *
 * This function has to be called within the scope of a WriteUnitOfWork with
 * a valid CollectionPtr reference to the oplog.
 *
 * @param records a vector of oplog records to be written. Records hold references
 * to unowned BSONObj data.
 * @param timestamps a vector of respective Timestamp objects for each oplog record.
 * @param oplogCollection collection to be written to.
 * @param finalOpTime the OpTime of the last oplog record.
 * @param wallTime the wall clock time of the last oplog record.
 * @param isAbortIndexBuild for tenant migration use only.
 */
void logOplogRecords(OperationContext* opCtx,
                     const NamespaceString& nss,
                     std::vector<Record>* records,
                     const std::vector<Timestamp>& timestamps,
                     const CollectionPtr& oplogCollection,
                     OpTime finalOpTime,
                     Date_t wallTime,
                     bool isAbortIndexBuild);

// Flush out the cached pointer to the oplog.
void clearLocalOplogPtr(ServiceContext* service);

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
void establishOplogCollectionForLogging(OperationContext* opCtx, const Collection* oplog);

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
    // This only being used in 'applyOps' command when sent by client.
    static constexpr StringData kRecoveringOplogApplicationMode = "Recovering"_sd;
    static constexpr StringData kStableRecoveringOplogApplicationMode = "StableRecovering"_sd;
    static constexpr StringData kUnstableRecoveringOplogApplicationMode = "UnstableRecovering"_sd;
    static constexpr StringData kSecondaryOplogApplicationMode = "Secondary"_sd;
    static constexpr StringData kApplyOpsCmdOplogApplicationMode = "ApplyOps"_sd;

    enum class Mode {
        // Used during the oplog application phase of the initial sync process.
        kInitialSync,

        // Used when we are applying oplog operations to recover the database state following an
        // clean/unclean shutdown, or when we are recovering from the oplog after we rollback to a
        // checkpoint.
        // If recovering from a unstable stable checkpoint.
        kUnstableRecovering,
        // If recovering from a stable checkpoint.~
        kStableRecovering,

        // Used when a secondary node is applying oplog operations from the primary during steady
        // state replication.
        kSecondary,

        // Used when we are applying operations as part of a direct client invocation of the
        // 'applyOps' command.
        kApplyOpsCmd
    };

    static bool inRecovering(Mode mode) {
        return mode == Mode::kUnstableRecovering || mode == Mode::kStableRecovering;
    }

    static StringData modeToString(Mode mode);

    static StatusWith<Mode> parseMode(const std::string& mode);

    // Server will crash on oplog application failure during recovery from stable checkpoint in the
    // test environment.
    static void checkOnOplogFailureForRecovery(OperationContext* opCtx,
                                               const mongo::NamespaceString& nss,
                                               const mongo::BSONObj& oplogEntry,
                                               const std::string& errorMsg);
};

inline std::ostream& operator<<(std::ostream& s, OplogApplication::Mode mode) {
    return (s << OplogApplication::modeToString(mode));
}

/**
 * Logs an oplog constraint violation and writes an entry into the health log.
 */
void logOplogConstraintViolation(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 OplogConstraintViolationEnum type,
                                 const std::string& operation,
                                 const BSONObj& opObj,
                                 boost::optional<Status> status);

/**
 * Used for applying from an oplog entry or grouped inserts.
 * @param opOrGroupedInserts a single oplog entry or grouped inserts to be applied.
 * @param alwaysUpsert convert some updates to upserts for idempotency reasons
 * @param mode specifies what oplog application mode we are in
 * @param incrementOpsAppliedStats is called whenever an op is applied.
 * Returns failure status if the op was an update that could not be applied.
 */
Status applyOperation_inlock(OperationContext* opCtx,
                             CollectionAcquisition& collectionAcquisition,
                             const OplogEntryOrGroupedInserts& opOrGroupedInserts,
                             bool alwaysUpsert,
                             OplogApplication::Mode mode,
                             bool isDataConsistent,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats = {});

/**
 * Take a command op and apply it locally
 * Used for applying from an oplog and for applyOps command.
 * Returns failure status if the op that could not be applied.
 */
Status applyCommand_inlock(OperationContext* opCtx,
                           const ApplierOperation& op,
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
 * contain the new optimes along with their terms.
 */
std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count);

inline OplogSlot getNextOpTime(OperationContext* opCtx) {
    auto slots = getNextOpTimes(opCtx, 1);
    invariant(slots.size() == 1);
    return slots.back();
}

using ApplyImportCollectionFn = std::function<void(OperationContext*,
                                                   const UUID&,
                                                   const NamespaceString&,
                                                   long long,
                                                   long long,
                                                   const BSONObj&,
                                                   const BSONObj&,
                                                   bool,
                                                   OplogApplication::Mode)>;

void registerApplyImportCollectionFn(ApplyImportCollectionFn func);

template <typename F>
auto writeConflictRetryWithLimit(OperationContext* opCtx,
                                 StringData opStr,
                                 const NamespaceStringOrUUID& nssOrUUID,
                                 F&& f) {
    return writeConflictRetry(opCtx, opStr, nssOrUUID, f, repl::writeConflictRetryLimit);
}

}  // namespace repl
}  // namespace mongo
