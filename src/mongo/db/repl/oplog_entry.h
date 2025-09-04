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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_base_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace MONGO_MOD_OPEN repl {

/**
 * The first oplog entry is a no-op with this message in its "msg" field.
 */
constexpr auto kInitiatingSetMsg = "initiating set"_sd;

/**
 * Field name of the newPrimaryMsg within the 'o' field in the new term no-op oplog entry.
 */
constexpr StringData kNewPrimaryMsgField = "msg"_sd;

/**
 * Message string passed in the new term no-op oplog entry after a primary has stepped up.
 */
constexpr StringData kNewPrimaryMsg = "new primary"_sd;

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
    };

    static ReplOperation parse(const BSONObj& bsonObject, const IDLParserContext& ctxt) {
        ReplOperation o;
        o.parseProtected(bsonObject, ctxt);
        return o;
    }

    static ReplOperation parseOwned(const BSONObj&& bsonObject, const IDLParserContext& ctxt) {
        ReplOperation o;
        o.parseProtected(bsonObject, ctxt);
        o.setAnchor(bsonObject);
        return o;
    }

    ReplOperation() = default;
    explicit ReplOperation(DurableReplOperation durableReplOp)
        : DurableReplOperation(std::move(durableReplOp)) {}

    const BSONObj& getPostImageDocumentKey() const {
        return _postImageDocumentKey;
    }

    void setPostImageDocumentKey(BSONObj value) {
        _postImageDocumentKey = std::move(value);
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
        DurableReplOperation::setStatementIds(stmtIds);
    }

    const std::vector<StmtId>& getStatementIds() const {
        return DurableReplOperation::getStatementIds();
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
    void setTid(boost::optional<mongo::TenantId> value) &;

    /**
     * Exports pre/post image information, if present, for writing to the image collection.
     *
     * Exported information includes both the image document and a flag to indicate if this
     * is for a pre or post image.
     * Does not fill in 'timestamp' - this will be filled in by OpObserverImpl after we have
     * written the corresponding applyOps oplog entry.
     *
     * Accepts an output parameter that for the image information that we expect
     * to be an uninitialized optional value. This output parameter may be set by a previous
     * call to this function. In this case, if this ReplOperation has an image, this will
     * result in an exception thrown.
     */
    using ImageBundle = struct {
        repl::RetryImageEnum imageKind;
        BSONObj imageDoc;
        Timestamp timestamp;
    };
    void extractPrePostImageForTransaction(boost::optional<ImageBundle>* image) const;

private:
    // Stores the post image _id + shard key values.
    BSONObj _postImageDocumentKey;

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
                                             const BSONObj& docKey,
                                             boost::optional<bool> isTimeseries = false);
    static ReplOperation makeUpdateOperation(NamespaceString nss,
                                             UUID uuid,
                                             const BSONObj& update,
                                             const BSONObj& criteria,
                                             boost::optional<bool> isTimeseries = false);
    static ReplOperation makeDeleteOperation(const NamespaceString& nss,
                                             UUID uuid,
                                             const BSONObj& docToDelete,
                                             boost::optional<bool> isTimeseries = false);

    /**
     * Generates the 'o' field of a 'create' OplogEntry.
     */
    static BSONObj makeCreateCollObject(const NamespaceString& collectionName,
                                        const mongo::CollectionOptions& options,
                                        const BSONObj& idIndex);

    /**
     * Attaches local catalog identifiers into the 'o2' field of a 'create' OplogEntry.
     */
    static BSONObj makeCreateCollObject2(const RecordId& catalogId,
                                         StringData ident,
                                         const boost::optional<std::string>& idIndexIdent,
                                         bool directoryPerDB,
                                         bool directoryForIndexes);

    static StatusWith<MutableOplogEntry> parse(const BSONObj& object);

    MutableOplogEntry() : OplogEntryBase() {}

    void setSessionId(boost::optional<LogicalSessionId> value) & {
        getOperationSessionInfo().setSessionId(std::move(value));
    }

    void setStatementIds(const std::vector<StmtId>& stmtIds) & {
        getDurableReplOperation().setStatementIds(stmtIds);
    }

    const std::vector<StmtId>& getStatementIds() const {
        return getDurableReplOperation().getStatementIds();
    }

    void setTxnNumber(boost::optional<std::int64_t> value) & {
        getOperationSessionInfo().setTxnNumber(std::move(value));
    }

    void setOpType(OpTypeEnum value) & {
        getDurableReplOperation().setOpType(std::move(value));
    }

    void setTid(boost::optional<mongo::TenantId> value) &;

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

    void setIsTimeseries() & {
        getDurableReplOperation().setIsTimeseries(true);
    }

    void setRecordId(RecordId rid) & {
        getDurableReplOperation().setRecordId(std::move(rid));
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

    void setCheckExistenceForDiffInsert() & {
        getDurableReplOperation().setCheckExistenceForDiffInsert(true);
    }

    void setVersionContext(boost::optional<VersionContext> value) {
        getDurableReplOperation().setVersionContext(std::move(value));
    }

    /**
     * Same as setFromMigrate but only set when it is true.
     */
    void setFromMigrateIfTrue(bool value) & {
        if (value)
            setFromMigrate(value);
    }

    /**
     * ReplOperation and MutableOplogEntry mostly hold the same data,
     * but lack a common ancestor in C++. Due to the details of IDL,
     * there is no generated C++ link between the two hierarchies.
     *
     * The primary difference between the two types is the OpTime
     * stored in OplogEntryBase that is absent in DurableReplOperation.
     *
     * This type conversion is useful in contexts when the two type
     * hierarchies should be interchangeable, like with internal tx's.
     * See: logMutableOplogEntry() in op_observer_impl.cpp.
     *
     * OplogEntryBase<>-------DurableReplOperation
     *        ^                       ^
     *        |                       |
     * MutableOplogEntry       ReplOperation
     *        ^
     *        |
     * DurableOplogEntry
     */
    ReplOperation toReplOperation() const noexcept;
};

struct DurableOplogEntryParams {
    OpTime opTime;
    OpTypeEnum opType;
    NamespaceString nss;
    boost::optional<StringData> container;
    boost::optional<UUID> uuid;
    boost::optional<bool> fromMigrate;
    boost::optional<bool> checkExistenceForDiffInsert;
    boost::optional<VersionContext> versionContext;
    int version;
    BSONObj oField;
    boost::optional<BSONObj> o2Field;
    OperationSessionInfo sessionInfo;
    boost::optional<bool> isUpsert;
    Date_t wallClockTime;
    std::vector<StmtId> statementIds;
    boost::optional<OpTime> prevWriteOpTimeInTransaction;
    boost::optional<OpTime> preImageOpTime;
    boost::optional<OpTime> postImageOpTime;
    boost::optional<ShardId> destinedRecipient;
    boost::optional<Value> idField;
    boost::optional<RetryImageEnum> needsRetryImage;
};

/**
 * A parsed oplog entry that privately inherits from the MutableOplogEntry.
 * This class is immutable. All setters are hidden.
 */
class DurableOplogEntry : private MutableOplogEntry {
public:
    // Make field names accessible.
    using MutableOplogEntry::k_idFieldName;
    using MutableOplogEntry::kCheckExistenceForDiffInsertFieldName;
    using MutableOplogEntry::kContainerFieldName;
    using MutableOplogEntry::kDestinedRecipientFieldName;
    using MutableOplogEntry::kDurableReplOperationFieldName;
    using MutableOplogEntry::kFromMigrateFieldName;
    using MutableOplogEntry::kIsTimeseriesFieldName;
    using MutableOplogEntry::kMultiOpTypeFieldName;
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
    using MutableOplogEntry::kVersionContextFieldName;
    using MutableOplogEntry::kVersionFieldName;
    using MutableOplogEntry::kWallClockTimeFieldName;

    // Make serialize() and getters accessible.
    using MutableOplogEntry::get_id;
    using MutableOplogEntry::getCheckExistenceForDiffInsert;
    using MutableOplogEntry::getContainer;
    using MutableOplogEntry::getDestinedRecipient;
    using MutableOplogEntry::getDurableReplOperation;
    using MutableOplogEntry::getFromMigrate;
    using MutableOplogEntry::getIsTimeseries;
    using MutableOplogEntry::getMultiOpType;
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
    using MutableOplogEntry::getVersionContext;
    using MutableOplogEntry::getWallClockTime;
    using MutableOplogEntry::serialize;

    // Make helper functions accessible.
    using MutableOplogEntry::getOpTime;
    using MutableOplogEntry::makeDeleteOperation;
    using MutableOplogEntry::makeInsertOperation;
    using MutableOplogEntry::makeUpdateOperation;

    // Get the in-memory size in bytes of a ReplOperation.
    static size_t getDurableReplOperationSize(const DurableReplOperation& op);

    static StatusWith<DurableOplogEntry> parse(const BSONObj& object);

    DurableOplogEntry(OpTime opTime,
                      OpTypeEnum opType,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& uuid,
                      const boost::optional<bool>& fromMigrate,
                      const boost::optional<bool>& checkExistenceForDiffInsert,
                      const boost::optional<VersionContext>& versionContext,
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

    explicit DurableOplogEntry(const DurableOplogEntryParams& p);

    // DEPRECATED: This constructor can throw. Use static parse method instead.
    explicit DurableOplogEntry(BSONObj raw);

    DurableOplogEntry() = delete;

    /**
     * Returns if the oplog entry is for a command operation.
     */
    bool isCommand() const;

    /**
     * Returns if the applyOps oplog entry is linked through its prevOpTime field as part of a
     * transaction, rather than as a retryable write or stand-alone applyOps.  Valid only for
     * applyOps entries.
     */
    bool applyOpsIsLinkedTransactionally() const;

    /**
     * Returns if the oplog entry is part of a transaction, whether an applyOps, a prepare, or
     * a commit.
     */
    bool isInTransaction() const;

    /**
     * Returns if the oplog entry is part of a transaction that has not yet been prepared or
     * committed.  The actual "prepare" or "commit" oplog entries do not have a "partialTxn" field
     * and so this method will always return false for them.
     */
    bool isPartialTransaction() const {
        if (getCommandType() != CommandTypeEnum::kApplyOps) {
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
        return getCommandType() == CommandTypeEnum::kCommitTransaction;
    }

    /**
     * Returns if this is a prepared 'abortTransaction' oplog entry.
     */
    bool isPreparedAbort() const {
        // Normally an 'abortTransaction' oplog entry represents an aborted prepared transaction.
        // However during stepup, if the secondary sees that it didn't replicate the full oplog
        // chain of a large unprepared transcation, it will abort this transaction and write an
        // 'abortTransaction' oplog entry, even though it is an unprepared transcation. In this
        // case, the entry will have a null prevOpTime.
        if (getCommandType() != CommandTypeEnum::kAbortTransaction) {
            return false;
        }
        auto prevOptime = getPrevWriteOpTimeInTransaction();
        return prevOptime && !prevOptime->isNull();
    }

    /**
     * Returns if this is a prepared 'commitTransaction' or 'abortTransaction' oplog entry.
     */
    bool isPreparedCommitOrAbort() const {
        return isPreparedCommit() || isPreparedAbort();
    }

    /**
     * Returns if this is a prepared transaction command oplog entry, i.e. prepareTransaction,
     * commitTransaction or abortTransaction.
     */
    bool isPreparedTransactionCommand() const {
        return isCommand() && (isPreparedCommit() || isPreparedAbort() || shouldPrepare());
    }

    /**
     * Returns whether the oplog entry represents an applyOps which is a self-contained atomic
     * operation, or the last applyOps of an unprepared transaction, as opposed to part of a
     * prepared transaction or a non-final applyOps in a transaction.
     */
    bool isTerminalApplyOps() const {
        return getCommandType() == CommandTypeEnum::kApplyOps && !shouldPrepare() &&
            !isPartialTransaction();
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
     * Returns whether the oplog entry represents the new Primary Noop Entry.
     */
    bool isNewPrimaryNoop() const;

    /**
     * Returns true if the oplog entry is for a CRUD operation.
     */
    static bool isCrudOpType(OpTypeEnum opType);
    bool isCrudOpType() const;

    /**
     * Returns true if the oplog entry is for a container operation.
     */
    static bool isContainerOpType(OpTypeEnum opType);
    bool isContainerOpType() const;

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
    CommandTypeEnum getCommandType() const;

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
    CommandTypeEnum _commandType = CommandTypeEnum::kNotCommand;
};

CommandTypeEnum parseCommandType(const BSONObj& objectField);

/**
 * Data structure that holds a DurableOplogEntry and other different run time state variables.
 */
class OplogEntry {
public:
    using CommandType = CommandTypeEnum;
    static constexpr auto k_idFieldName = DurableOplogEntry::k_idFieldName;
    static constexpr auto kDestinedRecipientFieldName =
        DurableOplogEntry::kDestinedRecipientFieldName;
    static constexpr auto kDurableReplOperationFieldName =
        DurableOplogEntry::kDurableReplOperationFieldName;
    static constexpr auto kFromMigrateFieldName = DurableOplogEntry::kFromMigrateFieldName;
    static constexpr auto kCheckExistenceForDiffInsertFieldName =
        DurableOplogEntry::kCheckExistenceForDiffInsertFieldName;
    static constexpr auto kVersionContextFieldName = DurableOplogEntry::kVersionContextFieldName;
    static constexpr auto kMultiOpTypeFieldName = DurableOplogEntry::kMultiOpTypeFieldName;
    static constexpr auto kTidFieldName = DurableOplogEntry::kTidFieldName;
    static constexpr auto kNssFieldName = DurableOplogEntry::kNssFieldName;
    static constexpr auto kObject2FieldName = DurableOplogEntry::kObject2FieldName;
    static constexpr auto kObjectFieldName = DurableOplogEntry::kObjectFieldName;
    static constexpr auto kIsTimeseriesFieldName = DurableOplogEntry::kIsTimeseriesFieldName;
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

    static boost::optional<TenantId> parseTid(const BSONObj& object);

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
    const std::vector<StmtId>& getStatementIds() const&;
    const OperationSessionInfo& getOperationSessionInfo() const;
    const boost::optional<mongo::LogicalSessionId>& getSessionId() const;
    boost::optional<std::int64_t> getTxnNumber() const;
    const DurableReplOperation& getDurableReplOperation() const;
    mongo::repl::OpTypeEnum getOpType() const;
    const boost::optional<mongo::TenantId>& getTid() const;
    const mongo::NamespaceString& getNss() const;
    const boost::optional<mongo::UUID>& getUuid() const;
    boost::optional<StringData> getContainer() const;
    const mongo::BSONObj& getObject() const;
    const boost::optional<mongo::BSONObj>& getObject2() const;
    boost::optional<bool> getIsTimeseries() const;
    boost::optional<bool> getUpsert() const;
    const boost::optional<mongo::repl::OpTime>& getPreImageOpTime() const;
    const boost::optional<mongo::ShardId>& getDestinedRecipient() const;
    const mongo::Timestamp& getTimestamp() const;
    boost::optional<std::int64_t> getTerm() const;
    const mongo::Date_t& getWallClockTime() const;
    std::int64_t getVersion() const;
    boost::optional<bool> getFromMigrate() const&;
    bool getCheckExistenceForDiffInsert() const&;
    const boost::optional<VersionContext>& getVersionContext() const;
    const boost::optional<mongo::repl::OpTime>& getPrevWriteOpTimeInTransaction() const&;
    const boost::optional<mongo::repl::OpTime>& getPostImageOpTime() const&;
    boost::optional<MultiOplogEntryType> getMultiOpType() const&;

    OpTime getOpTime() const;
    bool isCommand() const;
    bool applyOpsIsLinkedTransactionally() const;
    bool isInTransaction() const;
    bool isPartialTransaction() const;
    bool isEndOfLargeTransaction() const;
    bool isPreparedCommit() const;
    bool isPreparedAbort() const;
    bool isPreparedCommitOrAbort() const;
    bool isPreparedTransactionCommand() const;
    bool isTerminalApplyOps() const;
    bool isSingleOplogEntryTransaction() const;
    bool isSingleOplogEntryTransactionWithCommand() const;
    bool isNewPrimaryNoop() const;

    /**
     * Returns whether this oplog entry contains a DDL operation. Used to determine whether to
     * log the entry.
     */
    bool shouldLogAsDDLOperation() const;

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

    /**
     * Overrides the needsRetryImage value from DurableOplogEntry with boost::none
     */
    void clearNeedsRetryImage();

    /**
     * Returns the retry image setting from the original DurableOplogEntry, unless it
     * has been suppressed
     */
    boost::optional<RetryImageEnum> getNeedsRetryImage() const;


    bool isCrudOpType() const;
    bool isContainerOpType() const;
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

    // We allow this to be suppressed during secondary oplog application.
    boost::optional<RetryImageEnum> _needsRetryImage;
};

/**
 * Oplog entry document parser. This parser can parse only the key fields. It parses fields on
 * demand. This parser should be used only in cases when to be parsed oplog entry data structure
 * version may not match the one that is used by the current server version (this can happen with
 * past or future versions of oplog entries), otherwise 'OplogEntry::parse()' is supposed to be
 * used.
 */
class OplogEntryParserNonStrict {
public:
    /**
     * Constructs the parser with to be parsed oplog entry document 'oplogEntry'.
     */
    OplogEntryParserNonStrict(const BSONObj& oplogEntry);

    /**
     * Parses and returns "opTime" field.
     */
    repl::OpTime getOpTime() const;

    /**
     * Parses and returns the type of operation field.
     */
    repl::OpTypeEnum getOpType() const;

    /**
     * Parses and returns the "operation applied" field.
     */
    BSONObj getObject() const;

private:
    // Oplog entry as BSON object to be parsed.
    const BSONObj _oplogEntryObject;
};

std::ostream& operator<<(std::ostream& s, const DurableOplogEntry& o);
std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const DurableOplogEntry& lhs, const DurableOplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.getRaw() == rhs.getRaw());
}

bool operator==(const OplogEntry& lhs, const OplogEntry& rhs);

std::ostream& operator<<(std::ostream& s, const ReplOperation& o);

}  // namespace MONGO_MOD_OPEN repl
}  // namespace mongo
