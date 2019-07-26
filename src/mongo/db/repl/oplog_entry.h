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
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/apply_ops_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {

/**
 * A parsed DurableReplOperation along with information about the operation that should only exist
 * in-memory.
 *
 * ReplOperation should always be used over DurableReplOperation when passing around ReplOperations
 * in server code.
 */

class ReplOperation : public DurableReplOperation {
public:
    static ReplOperation parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject) {
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

private:
    BSONObj _preImageDocumentKey;
};

/**
 * Mutable class used on primary to build up oplog entries progressively.
 */
class MutableOplogEntry : public OplogEntryBase {
public:
    // Current oplog version, should be the value of the v field in all oplog entries.
    static const int kOplogVersion;

    // Helpers to generate ReplOperation.
    static ReplOperation makeInsertOperation(const NamespaceString& nss,
                                             boost::optional<UUID> uuid,
                                             const BSONObj& docToInsert);
    static ReplOperation makeUpdateOperation(const NamespaceString nss,
                                             boost::optional<UUID> uuid,
                                             const BSONObj& update,
                                             const BSONObj& criteria);
    static ReplOperation makeDeleteOperation(const NamespaceString& nss,
                                             boost::optional<UUID> uuid,
                                             const BSONObj& docToDelete);

    static StatusWith<MutableOplogEntry> parse(const BSONObj& object);

    MutableOplogEntry() : OplogEntryBase() {}

    void setSessionId(boost::optional<LogicalSessionId> value) & {
        getOperationSessionInfo().setSessionId(std::move(value));
    }

    void setTxnNumber(boost::optional<std::int64_t> value) & {
        getOperationSessionInfo().setTxnNumber(std::move(value));
    }

    void setOpType(OpTypeEnum value) & {
        getDurableReplOperation().setOpType(std::move(value));
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

    /**
     * Sets the OpTime of the oplog entry.
     */
    void setOpTime(const OpTime& opTime) &;

    /**
     * Returns the OpTime of the oplog entry.
     */
    OpTime getOpTime() const;

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
class OplogEntry : private MutableOplogEntry {
public:
    // Make field names accessible.
    using MutableOplogEntry::k_idFieldName;
    using MutableOplogEntry::kDurableReplOperationFieldName;
    using MutableOplogEntry::kFromMigrateFieldName;
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
    using MutableOplogEntry::kStatementIdFieldName;
    using MutableOplogEntry::kTermFieldName;
    using MutableOplogEntry::kTimestampFieldName;
    using MutableOplogEntry::kTxnNumberFieldName;
    using MutableOplogEntry::kUpsertFieldName;
    using MutableOplogEntry::kUuidFieldName;
    using MutableOplogEntry::kVersionFieldName;
    using MutableOplogEntry::kWallClockTimeFieldName;

    // Make serialize(), toBSON() and getters accessible.
    using MutableOplogEntry::get_id;
    using MutableOplogEntry::getDurableReplOperation;
    using MutableOplogEntry::getFromMigrate;
    using MutableOplogEntry::getHash;
    using MutableOplogEntry::getNss;
    using MutableOplogEntry::getObject;
    using MutableOplogEntry::getObject2;
    using MutableOplogEntry::getOperationSessionInfo;
    using MutableOplogEntry::getOpType;
    using MutableOplogEntry::getPostImageOpTime;
    using MutableOplogEntry::getPreImageOpTime;
    using MutableOplogEntry::getPrevWriteOpTimeInTransaction;
    using MutableOplogEntry::getSessionId;
    using MutableOplogEntry::getStatementId;
    using MutableOplogEntry::getTerm;
    using MutableOplogEntry::getTimestamp;
    using MutableOplogEntry::getTxnNumber;
    using MutableOplogEntry::getUpsert;
    using MutableOplogEntry::getUuid;
    using MutableOplogEntry::getVersion;
    using MutableOplogEntry::getWallClockTime;
    using MutableOplogEntry::serialize;
    using MutableOplogEntry::toBSON;

    // Make helper functions accessible.
    using MutableOplogEntry::getOpTime;
    using MutableOplogEntry::makeDeleteOperation;
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
        kConvertToCapped,
        kCreateIndexes,
        kStartIndexBuild,
        kCommitIndexBuild,
        kAbortIndexBuild,
        kDropIndexes,
        kCommitTransaction,
        kAbortTransaction,
    };

    // Get the in-memory size in bytes of a ReplOperation.
    static size_t getDurableReplOperationSize(const DurableReplOperation& op);

    static StatusWith<OplogEntry> parse(const BSONObj& object);

    OplogEntry(OpTime opTime,
               const boost::optional<long long> hash,
               OpTypeEnum opType,
               const NamespaceString& nss,
               const boost::optional<UUID>& uuid,
               const boost::optional<bool>& fromMigrate,
               int version,
               const BSONObj& oField,
               const boost::optional<BSONObj>& o2Field,
               const OperationSessionInfo& sessionInfo,
               const boost::optional<bool>& isUpsert,
               const boost::optional<mongo::Date_t>& wallClockTime,
               const boost::optional<StmtId>& statementId,
               const boost::optional<OpTime>& prevWriteOpTimeInTransaction,
               const boost::optional<OpTime>& preImageOpTime,
               const boost::optional<OpTime>& postImageOpTime);

    // DEPRECATED: This constructor can throw. Use static parse method instead.
    explicit OplogEntry(BSONObj raw);

    OplogEntry() = delete;

    // This member is not parsed from the BSON and is instead populated by fillWriterVectors.
    bool isForCappedCollection = false;

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
     * Returns if the oplog entry is for a CRUD operation.
     */
    static bool isCrudOpType(OpTypeEnum opType);
    bool isCrudOpType() const;

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
     * Returns the size of the original document used to create this OplogEntry.
     */
    int getRawObjSizeBytes() const;

    /**
     * Returns the original document used to create this OplogEntry.
     */
    const BSONObj& getRaw() const {
        return _raw;
    }

    /**
     * Serializes the oplog entry to a string.
     */
    std::string toString() const;

private:
    BSONObj _raw;  // Owned.
    CommandType _commandType = CommandType::kNotCommand;
};

std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const OplogEntry& lhs, const OplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.getRaw() == rhs.getRaw());
}

std::ostream& operator<<(std::ostream& s, const ReplOperation& o);

}  // namespace repl
}  // namespace mongo
