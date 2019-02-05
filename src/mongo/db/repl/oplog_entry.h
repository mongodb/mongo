
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
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"

namespace mongo {
namespace repl {

/**
 * A parsed oplog entry that inherits from the OplogEntryBase parsed by the IDL.
 * This class is immutable.
 */
class OplogEntry : public OplogEntryBase {
public:
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

    // Get the in-memory size in bytes of a ReplOperation.
    static size_t getReplOperationSize(const ReplOperation& op);

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
     * Returns the document representing the operation to apply.
     * For commands and insert/delete operations, this will be the document in the 'o' field.
     * For update operations, this will be the document in the 'o2' field.
     * An empty document returned by this function indicates that we have a malformed OplogEntry.
     */
    BSONObj getOperationToApply() const;

    /**
     * Returns the type of command of the oplog entry. Must be called on a command op.
     */
    CommandType getCommandType() const;

    /**
     * Returns the size of the original document used to create this OplogEntry.
     */
    int getRawObjSizeBytes() const;

    /**
     * Returns the OpTime of the oplog entry.
     */
    OpTime getOpTime() const;

    /**
     * Serializes the oplog entry to a string.
     */
    std::string toString() const;

    // TODO (SERVER-29200): make `raw` private. Do not add more direct uses of `raw`.
    BSONObj raw;  // Owned.

private:
    CommandType _commandType = CommandType::kNotCommand;
};

std::ostream& operator<<(std::ostream& s, const OplogEntry& o);

inline bool operator==(const OplogEntry& lhs, const OplogEntry& rhs) {
    return SimpleBSONObjComparator::kInstance.evaluate(lhs.raw == rhs.raw);
}

std::ostream& operator<<(std::ostream& s, const ReplOperation& o);

}  // namespace repl
}  // namespace mongo
