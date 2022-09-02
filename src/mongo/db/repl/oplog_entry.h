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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
namespace repl {

namespace variant_util {
template <typename T>
std::vector<T> toVector(boost::optional<stdx::variant<T, std::vector<T>>> optVals) {
    if (!optVals) {
        return {};
    }
    return stdx::visit(OverloadedVisitor{[](T val) { return std::vector<T>{val}; },
                                         [](const std::vector<T>& vals) { return vals; }},
                       *optVals);
}
}  // namespace variant_util

/**
 * The first oplog entry is a no-op with this message in its "msg" field.
 */
constexpr auto kInitiatingSetMsg = "initiating set"_sd;

/**
 * A parsed DurableReplOperation along with information about the operation that should only exist
 * in-memory.
 *
 * ReplOperation should always be used over DurableReplOperation when passing around ReplOperations
 * in server code.
 */

class ReplOperation : public DurableReplOperation {
public:
    /**
     * The way the change stream pre-images are recorded upon update/replace/delete operation.
     */
    enum class ChangeStreamPreImageRecordingMode {
        // The pre-image is not recorded.
        kOff,

        // The pre-image is recorded in the change stream pre-images collection.
        kPreImagesCollection,

        // The pre-image is recorded in the oplog as a separate entry.
        kOplog,
    };

    static ReplOperation parse(const IDLParserContext& ctxt, const BSONObj& bsonObject) {
        ReplOperation o;
        o.parseProtected(ctxt, bsonObject);
        return o;
    }
    const BSONObj& getPreImageDocumentKey() const {
        return _preImageDocumentKey;
    }
    void setPreImageDocumentKey(BSONObj value) {
        _preImageDocumentKey = std::move(value);
    }

    const BSONObj& getPreImage() const {
        return _fullPreImage;
    }

    void setPreImage(BSONObj value) {
        if (!_fullPreImage.isEmpty()) {
            uassert(6054003,
                    "Cannot set pre-image more than once",
                    _fullPreImage.woCompare(value) == 0);
            return;
        }
        _fullPreImage = std::move(value);
    }

    const BSONObj& getPostImage() const {
        return _fullPostImage;
    }

    void setPostImage(BSONObj value) {
        if (!_fullPostImage.isEmpty()) {
            uassert(6054004,
                    "Cannot set post-image more than once",
                    _fullPostImage.woCompare(value) == 0);
            return;
        }
        _fullPostImage = std::move(value);
    }

    /**
     * Returns the change stream pre-images recording mode applied for this operation.
     */
    ChangeStreamPreImageRecordingMode getChangeStreamPreImageRecordingMode() const {
        return _preImageRecordingMode;
    }

    /**
     * Sets the change stream pre-images recording mode to apply for this operation.
     */
    void setChangeStreamPreImageRecordingMode(ChangeStreamPreImageRecordingMode value) {
        _preImageRecordingMode = value;
    }

    /**
     * Returns true if the change stream pre-image is recorded in a dedicated oplog entry for this
     * operation.
     */
    bool isChangeStreamPreImageRecordedInOplog() const {
        return ReplOperation::ChangeStreamPreImageRecordingMode::kOplog ==
            getChangeStreamPreImageRecordingMode();
    }

    /**
     * Returns true if the change stream pre-image is recorded in the change stream pre-images
     * collection for this operation.
     */
    bool isChangeStreamPreImageRecordedInPreImagesCollection() const {
        return ReplOperation::ChangeStreamPreImageRecordingMode::kPreImagesCollection ==
            getChangeStreamPreImageRecordingMode();
    }

    /**
     * Returns true if the operation is in a retryable internal transaction and pre-image must be
     * recorded for the operation.
     */
    bool isPreImageRecordedForRetryableInternalTransaction() const {
        return _preImageRecordedForRetryableInternalTransaction;
    }

    /**
     * Sets whether the operation is in a retryable internal transaction and pre-image must be
     * recorded for the operation.
     */
    void setPreImageRecordedForRetryableInternalTransaction(bool value = true) {
        _preImageRecordedForRetryableInternalTransaction = value;
    }

    /**
     * Sets the statement ids for this ReplOperation to 'stmtIds' if it does not contain any
     * kUninitializedStmtId (i.e. placeholder statement id).
     */
    void setInitializedStatementIds(const std::vector<StmtId>& stmtIds) & {
        if (std::count(stmtIds.begin(), stmtIds.end(), kUninitializedStmtId) > 0) {
            return;
        }
        if (stmtIds.size() > 1) {
            DurableReplOperation::setStatementIds({{stmtIds}});
        } else if (stmtIds.size() == 1) {
            DurableReplOperation::setStatementIds({{stmtIds.front()}});
        }
    }

    std::vector<StmtId> getStatementIds() const {
        return variant_util::toVector<StmtId>(DurableReplOperation::getStatementIds());
    }

    void setFromMigrateIfTrue(bool value) & {
        if (value)
            setFromMigrate(value);
    }

    /**
     * This function overrides the base class setTid() function for the sole purpose of satisfying
     * the FCV checks.  Once these are deprecated, we should remove this overridden function
     * entirely.
     */
    void setTid(boost::optional<mongo::TenantId> value) & {
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))
            DurableReplOperation::setTid(value);
    }

private:
    BSONObj _preImageDocumentKey;

    // Used for storing the pre-image and post-image for the operation in-memory regardless of where
    // the images should be persisted.
    BSONObj _fullPreImage;
    BSONObj _fullPostImage;

    // Change stream pre-image recording mode applied to this operation.
    ChangeStreamPreImageRecordingMode _preImageRecordingMode{
        ChangeStreamPreImageRecordingMode::kOff};

    // Whether a pre-image must be recorded for this operation since it is in a retryable internal
    // transaction.
    bool _preImageRecordedForRetryableInternalTransaction{false};
};

/**
 * Mutable class used on primary to build up oplog entries progressively.
 */
class MutableOplogEntry : public OplogEntryBase {
public:
    // Current oplog version, should be the value of the v field in all oplog entries.
    static constexpr int kOplogVersion = 2;

    // Helpers to generate ReplOperation.
    static ReplOperation makeInsertOperation(const NamespaceString& nss,
                                             UUID uuid,
                                             const BSONObj& docToInsert,
                                             const BSONObj& docKey);
    static ReplOperation makeUpdateOperation(NamespaceString nss,
                                             UUID uuid,
                                             const BSONObj& update,
                                             const BSONObj& criteria);
    static ReplOperation makeDeleteOperation(const NamespaceString& nss,
                                             UUID uuid,
                                             const BSONObj& docToDelete);
    static ReplOperation makeInsertGlobalIndexKeyOperation(const NamespaceString& indexNss,
                                                           UUID indexUuid,
                                                           const BSONObj& key,
                                                           const BSONObj& docKey);

    static ReplOperation makeCreateCommand(NamespaceString nss,
                                           const mongo::CollectionOptions& options,
                                           const BSONObj& idIndex);

    static ReplOperation makeCreateIndexesCommand(NamespaceString nss,
                                                  const UUID& uuid,
                                                  const BSONObj& indexDoc);

    static BSONObj makeCreateCollCmdObj(const NamespaceString& collectionName,
                                        const mongo::CollectionOptions& options,
                                        const BSONObj& idIndex);

    static StatusWith<MutableOplogEntry> parse(const BSONObj& object);

    MutableOplogEntry() : OplogEntryBase() {}

    void setSessionId(boost::optional<LogicalSessionId> value) & {
        getOperationSessionInfo().setSessionId(std::move(value));
    }

    void setStatementIds(const std::vector<StmtId>& stmtIds) & {
        if (stmtIds.empty()) {
            getDurableReplOperation().setStatementIds(boost::none);
        } else if (stmtIds.size() == 1) {
            getDurableReplOperation().setStatementIds({{stmtIds.front()}});
        } else {
            getDurableReplOperation().setStatementIds({{stmtIds}});
        }
    }

    std::vector<StmtId> getStatementIds() const {
        return variant_util::toVector<StmtId>(OplogEntryBase::getStatementIds());
    }

    void setTxnNumber(boost::optional<std::int64_t> value) & {
        getOperationSessionInfo().setTxnNumber(std::move(value));
    }

    void setOpType(OpTypeEnum value) & {
        getDurableReplOperation().setOpType(std::move(value));
    }

    void setTid(boost::optional<mongo::TenantId> value) & {
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            gFeatureFlagRequireTenantID.isEnabled(serverGlobalParams.featureCompatibility))
            getDurableReplOperation().setTid(std::move(value));
    }

    void setNss(NamespaceString value) & {
        getDurableReplOperation().setNss(std::move(value));
    }

    void setUuid(boost::optional<UUID> value) & {
        getDurableReplOperation().setUuid(std::move(value));
    }

    void setObject(BSONObj value) & {
        getDurableReplOperation().setObject(std::move(value));
    }

    void setObject2(boost::optional<BSONObj> value) & {
        getDurableReplOperation().setObject2(std::move(value));
    }

    void setUpsert(boost::optional<bool> value) & {
        getDurableReplOperation().setUpsert(std::move(value));
    }

    void setPreImageOpTime(boost::optional<OpTime> value) {
        getDurableReplOperation().setPreImageOpTime(std::move(value));
    }

    const boost::optional<OpTime>& getPreImageOpTime() const {
        return getDurableReplOperation().getPreImageOpTime();
    }

    void setPostImageOpTime(boost::optional<OpTime> value) {
        getDurableReplOperation().setPostImageOpTime(std::move(value));
    }

    const boost::optional<OpTime>& getPostImageOpTime() const {
        return getDurableReplOperation().getPostImageOpTime();
    }

    void setTimestamp(Timestamp value) & {
        getOpTimeBase().setTimestamp(std::move(value));
    }

    void setTerm(boost::optional<std::int64_t> value) & {
        getOpTimeBase().setTerm(std::move(value));
    }

    void setDestinedRecipient(boost::optional<ShardId> value) {
        getDurableReplOperation().setDestinedRecipient(std::move(value));
    }

    const boost::optional<ShardId>& getDestinedRecipient() const {
        return getDurableReplOperation().getDestinedRecipient();
    }

    void setNeedsRetryImage(boost::optional<RetryImageEnum> value) & {
        getDurableReplOperation().setNeedsRetryImage(value);
    }

    /**
     * Sets the OpTime of the oplog entry.
     */
    void setOpTime(const OpTime& opTime) &;

    /**
     * Returns the OpTime of the oplog entry.
     */
    OpTime getOpTime() const;

    void setFromMigrate(bool value) & {
        getDurableReplOperation().setFromMigrate(value);
    }

    /**
     * Same as setFromMigrate but only set when it is true.
     */
    void setFromMigrateIfTrue(bool value) & {
        if (value)
            setFromMigrate(value);
    }
};

/**
 * A parsed oplog entry that privately inherits from the MutableOplogEntry.
 * This class is immutable. All setters are hidden.
 */
class DurableOplogEntry : private MutableOplogEntry {
public:
    // Make field names accessible.
    using MutableOplogEntry::k_idFieldName;
    using MutableOplogEntry::kDestinedRecipientFieldName;
    using MutableOplogEntry::kDurableReplOperationFieldName;
    using MutableOplogEntry::kFromMigrateFieldName;
    using MutableOplogEntry::kFromTenantMigrationFieldName;
    using MutableOplogEntry::kHashFieldName;
    using MutableOplogEntry::kNssFieldName;
    using MutableOplogEntry::kObject2FieldName;
    using MutableOplogEntry::kObjectFieldName;
    using MutableOplogEntry::kOperationSessionInfoFieldName;
    using MutableOplogEntry::kOplogVersion;
    using MutableOplogEntry::kOpTypeFieldName;
    using MutableOplogEntry::kPostImageOpTimeFieldName;
    using MutableOplogEntry::kPreImageOpTimeFieldName;
    using MutableOplogEntry::kPrevWriteOpTimeInTransactionFieldName;
    using MutableOplogEntry::kSessionIdFieldName;
    using MutableOplogEntry::kStatementIdsFieldName;
    using MutableOplogEntry::kTermFieldName;
    using MutableOplogEntry::kTidFieldName;
    using MutableOplogEntry::kTimestampFieldName;
    using MutableOplogEntry::kTxnNumberFieldName;
    using MutableOplogEntry::kUpsertFieldName;
    using MutableOplogEntry::kUuidFieldName;
    using MutableOplogEntry::kVersionFieldName;
    using MutableOplogEntry::kWallClockTimeFieldName;

    // Make serialize() and getters accessible.
    using MutableOplogEntry::get_id;
    using MutableOplogEntry::getDestinedRecipient;
    using MutableOplogEntry::getDurableReplOperation;
    using MutableOplogEntry::getFromMigrate;
    using MutableOplogEntry::getFromTenantMigration;
    using MutableOplogEntry::getHash;
    using MutableOplogEntry::getNeedsRetryImage;
    using MutableOplogEntry::getNss;
    using MutableOplogEntry::getObject;
    using MutableOplogEntry::getObject2;
    using MutableOplogEntry::getOperationSessionInfo;
    using MutableOplogEntry::getOpType;
    using MutableOplogEntry::getPostImageOpTime;
    using MutableOplogEntry::getPreImageOpTime;
    using MutableOplogEntry::getPrevWriteOpTimeInTransaction;
    using MutableOplogEntry::getSessionId;
    using MutableOplogEntry::getStatementIds;
    using MutableOplogEntry::getTerm;
    using MutableOplogEntry::getTid;
    using MutableOplogEntry::getTimestamp;
    using MutableOplogEntry::getTxnNumber;
    using MutableOplogEntry::getUpsert;
    using MutableOplogEntry::getUuid;
    using MutableOplogEntry::getVersion;
    using MutableOplogEntry::getWallClockTime;
    using MutableOplogEntry::serialize;

    // Make helper functions accessible.
    using MutableOplogEntry::getOpTime;
    using MutableOplogEntry::makeCreateCommand;
    using MutableOplogEntry::makeCreateIndexesCommand;
    using MutableOplogEntry::makeDeleteOperation;
    using MutableOplogEntry::makeInsertGlobalIndexKeyOperation;
    using MutableOplogEntry::makeInsertOperation;
    using MutableOplogEntry::makeUpdateOperation;

    enum class CommandType {
        kNotCommand,
        kCreate,
        kRenameCollection,
        kDbCheck,
        kDrop,
        kCollMod,
        kApplyOps,
        kDropDatabase,
        kEmptyCapped,
        kCreateIndexes,
        kStartIndexBuild,
        kCommitIndexBuild,
        kAbortIndexBuild,
        kDropIndexes,
        kCommitTransaction,
        kAbortTransaction,
        kImportCollection,
        kCreateGlobalIndex,
        kDropGlobalIndex,
        kInsertGlobalIndexKey,
    };

    // Get the in-memory size in bytes of a ReplOperation.
    static size_t getDurableReplOperationSize(const DurableReplOperation& op);

    static StatusWith<DurableOplogEntry> parse(const BSONObj& object);

    DurableOplogEntry(OpTime opTime,
                      OpTypeEnum opType,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& uuid,
                      const boost::optional<bool>& fromMigrate,
                      int version,
                      const BSONObj& oField,
                      const boost::optional<BSONObj>& o2Field,
                      const OperationSessionInfo& sessionInfo,
                      const boost::optional<bool>& isUpsert,
                      const mongo::Date_t& wallClockTime,
                      const std::vector<StmtId>& statementIds,
                      const boost::optional<OpTime>& prevWriteOpTimeInTransaction,
                      const boost::optional<OpTime>& preImageOpTime,
                      const boost::optional<OpTime>& postImageOpTime,
                      const boost::optional<ShardId>& destinedRecipient,
                      const boost::optional<Value>& idField,
                      const boost::optional<RetryImageEnum>& needsRetryImage);

    // DEPRECATED: This constructor can throw. Use static parse method instead.
    explicit DurableOplogEntry(BSONObj raw);

    DurableOplogEntry() = delete;

    /**
     * Returns if the oplog entry is for a command operation.
     */
    bool isCommand() const;

    /**
     * Returns if the oplog entry is part of a transaction that has not yet been prepared or
     * committed.  The actual "prepare" or "commit" oplog entries do not have a "partialTxn" field
     * and so this method will always return false for them.
     */
    bool isPartialTransaction() const {
        if (getCommandType() != CommandType::kApplyOps) {
            return false;
        }
        return getObject()[ApplyOpsCommandInfoBase::kPartialTxnFieldName].booleanSafe();
    }

    /**
     * Returns whether if the oplog entry is the last applyOps in a multiple-entry transaction.
     */
    bool isEndOfLargeTransaction() const;

    /**
     * Returns if this is a prepared 'commitTransaction' oplog entry.
     */
    bool isPreparedCommit() const {
        return getCommandType() == DurableOplogEntry::CommandType::kCommitTransaction;
    }

    /**
     * Returns whether the oplog entry represents an applyOps which is a self-contained atomic
     * operation, or the last applyOps of an unprepared transaction, as opposed to part of a
     * prepared transaction or a non-final applyOps in a transaction.
     */
    bool isTerminalApplyOps() const {
        return getCommandType() == DurableOplogEntry::CommandType::kApplyOps && !shouldPrepare() &&
            !isPartialTransaction() && !getObject().getBoolField("prepare");
    }

    /**
     * Returns whether the oplog entry represents a single oplog entry transaction.
     */
    bool isSingleOplogEntryTransaction() const;

    /**
     * Returns whether the oplog entry represents an applyOps with a command inside. This is only
     * for transactions with only one oplog entry.
     */
    bool isSingleOplogEntryTransactionWithCommand() const;

    /**
     * Returns true if the oplog entry is for a CRUD operation.
     */
    static bool isCrudOpType(OpTypeEnum opType);
    bool isCrudOpType() const;

    /**
     * Returns true if the oplog entry is for an Update or Delete operation.
     */
    bool isUpdateOrDelete() const;

    /**
     * Returns true if the oplog entry is for a command related to indexes.
     * i.e createIndexes, dropIndexes, startIndexBuild, commitIndexBuild, abortIndexBuild.
     */
    bool isIndexCommandType() const;

    /**
     * Returns if the operation should be prepared. Must be called on an 'applyOps' entry.
     */
    bool shouldPrepare() const;

    /**
     * Returns the _id of the document being modified. Must be called on CRUD ops.
     */
    BSONElement getIdElement() const;

    /**
     * Returns the document representing the operation to apply. This is the 'o' field for all
     * operations, including updates. For updates this is not guaranteed to include the _id or the
     * shard key.
     */
    BSONObj getOperationToApply() const;

    /**
     * Returns an object containing the _id of the target document for a CRUD operation. In a
     * sharded cluster this object also contains the shard key. This object may contain more fields
     * in the target document than the _id and shard key.
     * For insert/delete operations, this will be the document in the 'o' field.
     * For update operations, this will be the document in the 'o2' field.
     * Should not be called for non-CRUD operations.
     */
    BSONObj getObjectContainingDocumentKey() const;

    /**
     * Returns the type of command of the oplog entry. If it is not a command, returns kNotCommand.
     */
    CommandType getCommandType() const;

    /**
     * Returns the size of the original document used to create this DurableOplogEntry.
     */
    int getRawObjSizeBytes() const;

    /**
     * Returns the original document used to create this DurableOplogEntry.
     */
    const BSONObj& getRaw() const {
        return _raw;
    }

    /**
     * Serializes the oplog entry to a string.
     */
    std::string toString() const;

    BSONObj toBSON() const {
        return _raw;
    }

private:
    BSONObj _raw;  // Owned.
    CommandType _commandType = CommandType::kNotCommand;
};

DurableOplogEntry::CommandType parseCommandType(const BSONObj& objectField);

/**
 * Data structure that holds a DurableOplogEntry and other different run time state variables.
 */
class OplogEntry {
public:
    using CommandType = DurableOplogEntry::CommandType;
    static constexpr auto k_idFieldName = DurableOplogEntry::k_idFieldName;
    static constexpr auto kDestinedRecipientFieldName =
        DurableOplogEntry::kDestinedRecipientFieldName;
    static constexpr auto kDurableReplOperationFieldName =
        DurableOplogEntry::kDurableReplOperationFieldName;
    static constexpr auto kFromMigrateFieldName = DurableOplogEntry::kFromMigrateFieldName;
    static constexpr auto kFromTenantMigrationFieldName =
        DurableOplogEntry::kFromTenantMigrationFieldName;
    static constexpr auto kHashFieldName = DurableOplogEntry::kHashFieldName;
    static constexpr auto kTidFieldName = DurableOplogEntry::kTidFieldName;
    static constexpr auto kNssFieldName = DurableOplogEntry::kNssFieldName;
    static constexpr auto kObject2FieldName = DurableOplogEntry::kObject2FieldName;
    static constexpr auto kObjectFieldName = DurableOplogEntry::kObjectFieldName;
    static constexpr auto kOperationSessionInfoFieldName =
        DurableOplogEntry::kOperationSessionInfoFieldName;
    static constexpr auto kOplogVersion = DurableOplogEntry::kOplogVersion;
    static constexpr auto kOpTypeFieldName = DurableOplogEntry::kOpTypeFieldName;
    static constexpr auto kPostImageOpTimeFieldName = DurableOplogEntry::kPostImageOpTimeFieldName;
    static constexpr auto kPreImageOpTimeFieldName = DurableOplogEntry::kPreImageOpTimeFieldName;
    static constexpr auto kPrevWriteOpTimeInTransactionFieldName =
        DurableOplogEntry::kPrevWriteOpTimeInTransactionFieldName;
    static constexpr auto kSessionIdFieldName = DurableOplogEntry::kSessionIdFieldName;
    static constexpr auto kStatementIdFieldName = DurableOplogEntry::kStatementIdsFieldName;
    static constexpr auto kTermFieldName = DurableOplogEntry::kTermFieldName;
    static constexpr auto kTimestampFieldName = DurableOplogEntry::kTimestampFieldName;
    static constexpr auto kTxnNumberFieldName = DurableOplogEntry::kTxnNumberFieldName;
    static constexpr auto kUpsertFieldName = DurableOplogEntry::kUpsertFieldName;
    static constexpr auto kUuidFieldName = DurableOplogEntry::kUuidFieldName;
    static constexpr auto kVersionFieldName = DurableOplogEntry::kVersionFieldName;
    static constexpr auto kWallClockTimeFieldName = DurableOplogEntry::kWallClockTimeFieldName;

    OplogEntry(DurableOplogEntry oplog);
    OplogEntry(const BSONObj& oplog);

    const DurableOplogEntry& getEntry() const {
        return _entry;
    }

    void setEntry(DurableOplogEntry oplog);

    /**
     * Note: will only parse fields included in DurableOplogEntry.
     */
    static StatusWith<OplogEntry> parse(const BSONObj& object);

    bool isForCappedCollection() const;
    void setIsForCappedCollection(bool isForCappedCollection);

    std::string toStringForLogging() const;

    /**
     * Returns the BSON representation for diagnostic purposes. To get a BSON meant for storing to
     * the oplog collection, use getEntry().toBSON() instead.
     */
    BSONObj toBSONForLogging() const;

    // Wrapper methods for DurableOplogEntry
    const boost::optional<mongo::Value>& get_id() const&;
    std::vector<StmtId> getStatementIds() const&;
    const OperationSessionInfo& getOperationSessionInfo() const;
    const boost::optional<mongo::LogicalSessionId>& getSessionId() const;
    boost::optional<std::int64_t> getTxnNumber() const;
    const DurableReplOperation& getDurableReplOperation() const;
    mongo::repl::OpTypeEnum getOpType() const;
    const boost::optional<mongo::TenantId>& getTid() const;
    const mongo::NamespaceString& getNss() const;
    const boost::optional<mongo::UUID>& getUuid() const;
    const mongo::BSONObj& getObject() const;
    const boost::optional<mongo::BSONObj>& getObject2() const;
    boost::optional<bool> getUpsert() const;
    const boost::optional<mongo::repl::OpTime>& getPreImageOpTime() const;
    const boost::optional<mongo::ShardId>& getDestinedRecipient() const;
    const mongo::Timestamp& getTimestamp() const;
    boost::optional<std::int64_t> getTerm() const;
    const mongo::Date_t& getWallClockTime() const;
    boost::optional<std::int64_t> getHash() const&;
    std::int64_t getVersion() const;
    boost::optional<bool> getFromMigrate() const&;
    const boost::optional<mongo::UUID>& getFromTenantMigration() const&;
    const boost::optional<mongo::repl::OpTime>& getPrevWriteOpTimeInTransaction() const&;
    const boost::optional<mongo::repl::OpTime>& getPostImageOpTime() const&;
    boost::optional<RetryImageEnum> getNeedsRetryImage() const;
    OpTime getOpTime() const;
    bool isCommand() const;
    bool isPartialTransaction() const;
    bool isEndOfLargeTransaction() const;
    bool isPreparedCommit() const;
    bool isTerminalApplyOps() const;
    bool isSingleOplogEntryTransaction() const;
    bool isSingleOplogEntryTransactionWithCommand() const;

    /**
     * Returns an index of this operation in the "applyOps" entry, if the operation is packed in the
     * "applyOps" entry. Otherwise returns 0.
     */
    uint64_t getApplyOpsIndex() const;

    void setApplyOpsIndex(uint64_t value);

    /**
     * Returns a timestamp of the "applyOps" entry, if this operation is packed in the "applyOps"
     * entry. Otherwise returns boost::none.
     */
    const boost::optional<mongo::Timestamp>& getApplyOpsTimestamp() const;

    void setApplyOpsTimestamp(boost::optional<mongo::Timestamp> value);

    /**
     * Returns wall clock time of the "applyOps" entry, if this operation is packed in the
     * "applyOps" entry. Otherwise returns boost::none.
     */
    const boost::optional<mongo::Date_t>& getApplyOpsWallClockTime() const;

    void setApplyOpsWallClockTime(boost::optional<mongo::Date_t> value);

    /**
     * Returns a timestamp to use for recording of a change stream pre-image in the change stream
     * pre-images collection. Returns a timestamp of the "applyOps" entry, if this operation is
     * packed in the "applyOps" entry. Otherwise returns a timestamp of this oplog entry.
     */
    mongo::Timestamp getTimestampForPreImage() const;

    /**
     * Returns a wall clock time to use for recording of a change stream pre-image in the change
     * stream pre-images collection. Returns a wall clock time of the "applyOps" entry, if this
     * operation is packed in the "applyOps" entry. Otherwise returns a wall clock time of this
     * oplog entry.
     */
    mongo::Date_t getWallClockTimeForPreImage() const;

    bool isCrudOpType() const;
    bool isUpdateOrDelete() const;
    bool isIndexCommandType() const;
    bool shouldPrepare() const;
    BSONElement getIdElement() const;
    BSONObj getOperationToApply() const;
    BSONObj getObjectContainingDocumentKey() const;
    OplogEntry::CommandType getCommandType() const;
    int getRawObjSizeBytes() const;

private:
    DurableOplogEntry _entry;

    // An index of this oplog entry in the associated "applyOps" oplog entry when this entry is
    // extracted from an "applyOps" oplog entry. Otherwise, the index value must be 0.
    uint64_t _applyOpsIndex{0};

    // A timestamp of the associated "applyOps" oplog entry when this oplog entry is extracted from
    // an "applyOps" oplog entry.
    boost::optional<Timestamp> _applyOpsTimestamp{boost::none};

    // Wall clock time of the associated "applyOps" oplog entry when this oplog entry is extracted
    // from an "applyOps" oplog entry.
    boost::optional<Date_t> _applyOpsWallClockTime{boost::none};

    bool _isForCappedCollection = false;
};

std::ostream& operator<<(std::ostream& s, const DurableOplogEntry& o);
std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const DurableOplogEntry& lhs, const DurableOplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.getRaw() == rhs.getRaw());
}

bool operator==(const OplogEntry& lhs, const OplogEntry& rhs);

std::ostream& operator<<(std::ostream& s, const ReplOperation& o);

}  // namespace repl
}  // namespace mongo
