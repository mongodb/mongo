// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

struct IndexBuildInfo;
namespace repl {
/**
 * Creates an OplogEntry with given parameters and preset defaults.
 */
OplogEntry makeOplogEntry(repl::OpTime opTime,
                          repl::OpTypeEnum opType,
                          NamespaceString nss,
                          BSONObj object,
                          boost::optional<BSONObj> object2 = boost::none,
                          OperationSessionInfo sessionInfo = {},
                          Date_t wallClockTime = Date_t(),
                          const std::vector<StmtId>& stmtIds = {},
                          boost::optional<UUID> uuid = boost::none,
                          boost::optional<OpTime> prevOpTime = boost::none);

/**
 * Creates an insert oplog entry with given optime and namespace.
 */
OplogEntry makeInsertDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToInsert);
/**
 * Creates a delete oplog entry with given optime and namespace.
 */
OplogEntry makeDeleteDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToDelete);
/**
 * Creates an update oplog entry with given optime and namespace.
 */
OplogEntry makeUpdateDocumentOplogEntry(OpTime opTime,
                                        const NamespaceString& nss,
                                        const BSONObj& documentToUpdate,
                                        const BSONObj& updatedDocument);

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         int64_t key,
                                         BSONBinData value);

OplogEntry makeContainerInsertOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         BSONBinData key,
                                         BSONBinData value);

OplogEntry makeContainerUpdateOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         int64_t key,
                                         BSONBinData value);

OplogEntry makeContainerUpdateOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         BSONBinData key,
                                         BSONBinData value);

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         int64_t key);

OplogEntry makeContainerDeleteOplogEntry(OpTime opTime,
                                         std::string_view containerIdent,
                                         BSONBinData key);

/**
 * Creates an index creation entry with given optime and namespace.
 */
OplogEntry makeCreateIndexOplogEntry(OpTime opTime,
                                     const NamespaceString& nss,
                                     const std::string& indexName,
                                     const BSONObj& keyPattern,
                                     const UUID& uuid,
                                     const BSONObj& options = {});
/**
 * Creates a two-phase index build start oplog entry with a given optime, namespace, and index
 * build UUID.
 */
OplogEntry makeStartIndexBuildOplogEntry(OpTime opTime,
                                         const NamespaceString& nss,
                                         const UUID& uuid,
                                         const UUID& indexBuildUUID,
                                         const IndexBuildInfo& indexBuildInfo,
                                         std::string_view indexIdent);

/**
 * Creates a two-phase index build commit oplog entry with a given optime, namespace, and index
 * build UUID.
 */
OplogEntry makeCommitIndexBuildOplogEntry(OpTime opTime,
                                          const NamespaceString& nss,
                                          const UUID& uuid,
                                          const UUID& indexBuildUUID,
                                          const IndexBuildInfo& indexBuildInfo);

/**
 * Creates an oplog entry for 'command' with the given 'optime', 'namespace' and optional 'uuid'.
 */
OplogEntry makeCommandOplogEntry(OpTime opTime,
                                 const NamespaceString& nss,
                                 const BSONObj& object,
                                 boost::optional<BSONObj> object2 = boost::none,
                                 boost::optional<UUID> uuid = boost::none);

/**
 * Creates an oplog entry for 'command' with the given 'optime', 'namespace' and session information
 */
OplogEntry makeCommandOplogEntryWithSessionInfoAndStmtIds(
    OpTime opTime,
    const NamespaceString& nss,
    const BSONObj& command,
    LogicalSessionId lsid,
    TxnNumber txnNum,
    std::vector<StmtId> stmtIds,
    boost::optional<OpTime> prevOpTime = boost::none);

/**
 * Creates an insert oplog entry with given optime, namespace and session info.
 */
OplogEntry makeInsertDocumentOplogEntryWithSessionInfo(OpTime opTime,
                                                       const NamespaceString& nss,
                                                       const BSONObj& documentToInsert,
                                                       OperationSessionInfo info);

OplogEntry makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
    OpTime opTime,
    const NamespaceString& nss,
    boost::optional<UUID> uuid,
    const BSONObj& documentToInsert,
    LogicalSessionId lsid,
    TxnNumber txnNum,
    const std::vector<StmtId>& stmtIds,
    boost::optional<OpTime> prevOpTime = boost::none);

BSONObj makeInsertApplyOpsEntry(const NamespaceString& nss, const UUID& uuid, const BSONObj& doc);

repl::MutableOplogEntry makeNoopMutableOplogEntry(const NamespaceString& nss,
                                                  UUID uuid,
                                                  const LogicalSessionId& lsid,
                                                  TxnNumber txnNumber,
                                                  const std::vector<StmtId>& stmtIds,
                                                  repl::OpTime prevOpTime);

repl::OplogEntry makeNoopOplogEntry(repl::OpTime opTime,
                                    const NamespaceString& nss,
                                    UUID uuid,
                                    const LogicalSessionId& lsid,
                                    TxnNumber txnNumber,
                                    const std::vector<StmtId>& stmtIds,
                                    repl::OpTime prevOpTime);

enum class ApplyOpsType { kPartial, kPrepare, kTerminal };

repl::MutableOplogEntry makeApplyOpsMutableOplogEntry(
    std::vector<repl::ReplOperation> ops,
    OperationSessionInfo sessionInfo,
    Date_t wallClockTime,
    const std::vector<StmtId>& stmtIds,
    boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    boost::optional<repl::MultiOplogEntryType> multiOpType = boost::none,
    boost::optional<ApplyOpsType> applyOpsType = ApplyOpsType::kTerminal);


repl::OplogEntry makeApplyOpsOplogEntry(
    repl::OpTime opTime,
    std::vector<repl::ReplOperation> ops,
    OperationSessionInfo sessionInfo,
    Date_t wallClockTime,
    const std::vector<StmtId>& stmtIds,
    boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
    boost::optional<repl::MultiOplogEntryType> multiOpType = boost::none,
    boost::optional<ApplyOpsType> applyOpsType = ApplyOpsType::kTerminal);


repl::OplogEntry makeCommitTransactionOplogEntry(
    repl::OpTime opTime,
    OperationSessionInfo sessionInfo,
    Timestamp commitTimestamp,
    boost::optional<repl::OpTime> prevWriteOpTimeInTransaction);

repl::OplogEntry makeAbortTransactionOplogEntry(
    repl::OpTime opTime,
    OperationSessionInfo sessionInfo,
    boost::optional<repl::OpTime> prevWriteOpTimeInTransaction);

/**
 * Creates a insert oplog entry without a recordId. Uses DurableOplogEntryParams to construct
 * the entry.
 */
OplogEntry makeInsertOplogEntry(OpTime opTime,
                                const NamespaceString& nss,
                                const UUID& uuid,
                                const BSONObj& docToInsert);

/**
 * Creates a delete oplog entry without a recordId. Uses DurableOplogEntryParams to construct
 * the entry.
 */
OplogEntry makeDeleteOplogEntry(OpTime opTime,
                                const NamespaceString& nss,
                                const UUID& uuid,
                                const BSONObj& docToDelete);

/**
 * Creates a insert oplog entry with the given recordId. Uses DurableOplogEntryParams to construct
 * the entry, then adds the recordId since it's not included in the params struct.
 */
OplogEntry makeInsertOplogEntryWithRecordId(OpTime opTime,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            const BSONObj& docToInsert,
                                            const RecordId& rid);

/**
 * Creates an update oplog entry with the given recordId.
 */
OplogEntry makeUpdateOplogEntryWithRecordId(OpTime opTime,
                                            const NamespaceString& nss,
                                            const BSONObj& documentToUpdate,
                                            const BSONObj& updatedDocument,
                                            const RecordId& rid);

/**
 * Creates an update oplog entry with the upsert flag set.
 */
OplogEntry makeUpdateOplogEntryWithUpsert(OpTime opTime,
                                          const NamespaceString& nss,
                                          const BSONObj& documentToUpdate,
                                          const BSONObj& updatedDocument);

/**
 * Creates an update oplog entry with both the upsert flag and recordId set.
 */
OplogEntry makeUpdateOplogEntryWithUpsertAndRecordId(OpTime opTime,
                                                     const NamespaceString& nss,
                                                     const BSONObj& documentToUpdate,
                                                     const BSONObj& updatedDocument,
                                                     const RecordId& rid);

/**
 * Creates a delete oplog entry with the given recordId. Uses DurableOplogEntryParams to construct
 * the entry, then adds the recordId since it's not included in the params struct.
 */
OplogEntry makeDeleteOplogEntryWithRecordId(OpTime opTime,
                                            const NamespaceString& nss,
                                            const UUID& uuid,
                                            const BSONObj& docToDelete,
                                            const RecordId& rid);

/**
 * Creates an update oplog entry with the given recordId and size metadata (m.sz).
 */
OplogEntry makeUpdateOplogEntryWithRecordIdAndSizeMetadata(OpTime opTime,
                                                           const NamespaceString& nss,
                                                           const BSONObj& documentToUpdate,
                                                           const BSONObj& updatedDocument,
                                                           const RecordId& rid,
                                                           int sizeDelta);

/**
 * Creates a delete oplog entry with the given recordId and size metadata (m.sz).
 */
OplogEntry makeDeleteOplogEntryWithRecordIdAndSizeMetadata(OpTime opTime,
                                                           const NamespaceString& nss,
                                                           const UUID& uuid,
                                                           const BSONObj& docToDelete,
                                                           const RecordId& rid,
                                                           int sizeDelta);

OplogEntry makeUpdateOplogEntryWithRecordIdWithoutSz(OpTime opTime,
                                                     const NamespaceString& nss,
                                                     const BSONObj& documentToUpdate,
                                                     const BSONObj& updatedDocument,
                                                     const RecordId& rid);

OplogEntry makeDeleteOplogEntryWithRecordIdWithoutSz(OpTime opTime,
                                                     const NamespaceString& nss,
                                                     const UUID& uuid,
                                                     const BSONObj& docToDelete,
                                                     const RecordId& rid);

/*
 * Returns a collection UUID.
 */
UUID getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Inserts a document into a collection at a specific recordId. Returns the RecordId where the
 * document was inserted.
 */
void insertDocumentAtRecordId(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const BSONObj& doc,
                              const RecordId& rid);

/**
 * Checks if a document exists at a specific recordId.
 */
bool documentExistsAtRecordId(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const RecordId& rid);

/**
 * Returns document at a specific recordId if it exists.
 */
boost::optional<BSONObj> documentAtRecordId(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const RecordId& rid);

template <typename T, bool enable>
class SetSteadyStateConstraints : public T {
protected:
    void setUp() override {
        T::setUp();
        _constraintsEnabled = oplogApplicationEnforcesSteadyStateConstraints.load();
        oplogApplicationEnforcesSteadyStateConstraints.store(enable);
    }

    void tearDown() override {
        oplogApplicationEnforcesSteadyStateConstraints.store(_constraintsEnabled);
        T::tearDown();
    }

private:
    bool _constraintsEnabled;
};
}  // namespace repl
}  // namespace mongo
