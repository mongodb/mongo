
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

namespace {

OplogEntry::CommandType parseCommandType(const BSONObj& objectField) {
    StringData commandString(objectField.firstElementFieldName());
    if (commandString == "create") {
        return OplogEntry::CommandType::kCreate;
    } else if (commandString == "renameCollection") {
        return OplogEntry::CommandType::kRenameCollection;
    } else if (commandString == "drop") {
        return OplogEntry::CommandType::kDrop;
    } else if (commandString == "collMod") {
        return OplogEntry::CommandType::kCollMod;
    } else if (commandString == "applyOps") {
        return OplogEntry::CommandType::kApplyOps;
    } else if (commandString == "dbCheck") {
        return OplogEntry::CommandType::kDbCheck;
    } else if (commandString == "dropDatabase") {
        return OplogEntry::CommandType::kDropDatabase;
    } else if (commandString == "emptycapped") {
        return OplogEntry::CommandType::kEmptyCapped;
    } else if (commandString == "convertToCapped") {
        return OplogEntry::CommandType::kConvertToCapped;
    } else if (commandString == "createIndexes") {
        return OplogEntry::CommandType::kCreateIndexes;
    } else if (commandString == "startIndexBuild") {
        return OplogEntry::CommandType::kStartIndexBuild;
    } else if (commandString == "commitIndexBuild") {
        return OplogEntry::CommandType::kCommitIndexBuild;
    } else if (commandString == "abortIndexBuild") {
        return OplogEntry::CommandType::kAbortIndexBuild;
    } else if (commandString == "dropIndexes") {
        return OplogEntry::CommandType::kDropIndexes;
    } else if (commandString == "deleteIndexes") {
        return OplogEntry::CommandType::kDropIndexes;
    } else if (commandString == "commitTransaction") {
        return OplogEntry::CommandType::kCommitTransaction;
    } else if (commandString == "abortTransaction") {
        return OplogEntry::CommandType::kAbortTransaction;
    } else {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Unknown oplog entry command type: " << commandString
                                << " Object field: "
                                << redact(objectField));
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns a document representing an oplog entry with the given fields.
 */
BSONObj makeOplogEntryDoc(OpTime opTime,
                          const boost::optional<long long> hash,
                          OpTypeEnum opType,
                          const NamespaceString& nss,
                          const boost::optional<UUID>& uuid,
                          const boost::optional<bool>& fromMigrate,
                          int64_t version,
                          const BSONObj& oField,
                          const boost::optional<BSONObj>& o2Field,
                          const OperationSessionInfo& sessionInfo,
                          const boost::optional<bool>& isUpsert,
                          const boost::optional<mongo::Date_t>& wallClockTime,
                          const boost::optional<StmtId>& statementId,
                          const boost::optional<OpTime>& prevWriteOpTimeInTransaction,
                          const boost::optional<OpTime>& preImageOpTime,
                          const boost::optional<OpTime>& postImageOpTime) {
    BSONObjBuilder builder;
    sessionInfo.serialize(&builder);
    builder.append(OplogEntryBase::kTimestampFieldName, opTime.getTimestamp());
    builder.append(OplogEntryBase::kTermFieldName, opTime.getTerm());
    builder.append(OplogEntryBase::kVersionFieldName, version);
    builder.append(OplogEntryBase::kOpTypeFieldName, OpType_serializer(opType));
    builder.append(OplogEntryBase::kNssFieldName, nss.toString());
    if (hash) {
        builder.append(OplogEntryBase::kHashFieldName, hash.get());
    }
    if (uuid) {
        uuid->appendToBuilder(&builder, OplogEntryBase::kUuidFieldName);
    }
    if (fromMigrate) {
        builder.append(OplogEntryBase::kFromMigrateFieldName, fromMigrate.get());
    }
    builder.append(OplogEntryBase::kObjectFieldName, oField);
    if (o2Field) {
        builder.append(OplogEntryBase::kObject2FieldName, o2Field.get());
    }
    if (isUpsert) {
        invariant(o2Field);
        builder.append(OplogEntryBase::kUpsertFieldName, isUpsert.get());
    }
    if (wallClockTime) {
        builder.append(OplogEntryBase::kWallClockTimeFieldName, wallClockTime.get());
    }
    if (statementId) {
        builder.append(OplogEntryBase::kStatementIdFieldName, statementId.get());
    }
    if (prevWriteOpTimeInTransaction) {
        const BSONObj localObject = prevWriteOpTimeInTransaction.get().toBSON();
        builder.append(OplogEntryBase::kPrevWriteOpTimeInTransactionFieldName, localObject);
    }
    if (preImageOpTime) {
        const BSONObj localObject = preImageOpTime.get().toBSON();
        builder.append(OplogEntryBase::kPreImageOpTimeFieldName, localObject);
    }
    if (postImageOpTime) {
        const BSONObj localObject = postImageOpTime.get().toBSON();
        builder.append(OplogEntryBase::kPostImageOpTimeFieldName, localObject);
    }
    return builder.obj();
}

}  // namespace

const int OplogEntry::kOplogVersion = 2;

// Static
ReplOperation OplogEntry::makeInsertOperation(const NamespaceString& nss,
                                              boost::optional<UUID> uuid,
                                              const BSONObj& docToInsert) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kInsert);
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToInsert.getOwned());
    return op;
}

ReplOperation OplogEntry::makeUpdateOperation(const NamespaceString nss,
                                              boost::optional<UUID> uuid,
                                              const BSONObj& update,
                                              const BSONObj& criteria) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kUpdate);
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(update.getOwned());
    op.setObject2(criteria.getOwned());
    return op;
}

ReplOperation OplogEntry::makeDeleteOperation(const NamespaceString& nss,
                                              boost::optional<UUID> uuid,
                                              const BSONObj& docToDelete) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kDelete);
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToDelete.getOwned());
    return op;
}

size_t OplogEntry::getReplOperationSize(const ReplOperation& op) {
    return sizeof(op) + op.getNss().size() + op.getObject().objsize() +
        (op.getObject2() ? op.getObject2()->objsize() : 0);
}

StatusWith<OplogEntry> OplogEntry::parse(const BSONObj& object) {
    try {
        return OplogEntry(object);
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
}

OplogEntry::OplogEntry(BSONObj rawInput) : raw(std::move(rawInput)) {
    raw = raw.getOwned();

    parseProtected(IDLParserErrorContext("OplogEntryBase"), raw);

    // Parse command type from 'o' and 'o2' fields.
    if (isCommand()) {
        _commandType = parseCommandType(getObject());
    }
}

OplogEntry::OplogEntry(OpTime opTime,
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
                       const boost::optional<OpTime>& postImageOpTime)
    : OplogEntry(makeOplogEntryDoc(opTime,
                                   hash,
                                   opType,
                                   nss,
                                   uuid,
                                   fromMigrate,
                                   version,
                                   oField,
                                   o2Field,
                                   sessionInfo,
                                   isUpsert,
                                   wallClockTime,
                                   statementId,
                                   prevWriteOpTimeInTransaction,
                                   preImageOpTime,
                                   postImageOpTime)) {}

bool OplogEntry::isCommand() const {
    return getOpType() == OpTypeEnum::kCommand;
}

// static
bool OplogEntry::isCrudOpType(OpTypeEnum opType) {
    switch (opType) {
        case OpTypeEnum::kInsert:
        case OpTypeEnum::kDelete:
        case OpTypeEnum::kUpdate:
            return true;
        case OpTypeEnum::kCommand:
        case OpTypeEnum::kNoop:
            return false;
    }
    MONGO_UNREACHABLE;
}

bool OplogEntry::isCrudOpType() const {
    return isCrudOpType(getOpType());
}

bool OplogEntry::shouldPrepare() const {
    return getPrepare() && *getPrepare();
}

BSONElement OplogEntry::getIdElement() const {
    invariant(isCrudOpType());
    if (getOpType() == OpTypeEnum::kUpdate) {
        // We cannot use getOperationToApply() here because the BSONObj will go out out of scope
        // after we return the BSONElement.
        return getObject2()->getField("_id");
    } else {
        return getObject()["_id"];
    }
}

BSONObj OplogEntry::getOperationToApply() const {
    if (getOpType() != OpTypeEnum::kUpdate) {
        return getObject();
    }

    if (auto optionalObj = getObject2()) {
        return *optionalObj;
    }

    return {};
}

OplogEntry::CommandType OplogEntry::getCommandType() const {
    return _commandType;
}

int OplogEntry::getRawObjSizeBytes() const {
    return raw.objsize();
}

OpTime OplogEntry::getOpTime() const {
    long long term = OpTime::kUninitializedTerm;
    if (getTerm()) {
        term = getTerm().get();
    }
    return OpTime(getTimestamp(), term);
}

std::string OplogEntry::toString() const {
    return raw.toString();
}

std::ostream& operator<<(std::ostream& s, const OplogEntry& o) {
    return s << o.toString();
}

std::ostream& operator<<(std::ostream& s, const ReplOperation& o) {
    return s << o.toBSON().toString();
}

}  // namespace repl
}  // namespace mongo
