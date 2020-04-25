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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_entry.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/time_support.h"

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
                                << " Object field: " << redact(objectField));
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
                          const mongo::Date_t& wallClockTime,
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
    builder.append(OplogEntryBase::kWallClockTimeFieldName, wallClockTime);
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

const int MutableOplogEntry::kOplogVersion = 2;

// Static
ReplOperation MutableOplogEntry::makeInsertOperation(const NamespaceString& nss,
                                                     UUID uuid,
                                                     const BSONObj& docToInsert) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kInsert);
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToInsert.getOwned());
    return op;
}

BSONObj MutableOplogEntry::makeCreateCollCmdObj(const NamespaceString& collectionName,
                                                const CollectionOptions& options,
                                                const BSONObj& idIndex) {
    BSONObjBuilder b;
    b.append("create", collectionName.coll().toString());
    {
        // Don't store the UUID as part of the options, but instead only at the top level
        CollectionOptions optionsToStore = options;
        optionsToStore.uuid.reset();
        b.appendElements(optionsToStore.toBSON());
    }

    // Include the full _id index spec in the oplog for index versions >= 2.
    if (!idIndex.isEmpty()) {
        auto versionElem = idIndex[IndexDescriptor::kIndexVersionFieldName];
        invariant(versionElem.isNumber());
        if (IndexDescriptor::IndexVersion::kV2 <=
            static_cast<IndexDescriptor::IndexVersion>(versionElem.numberInt())) {
            b.append("idIndex", idIndex);
        }
    }

    return b.obj();
}

ReplOperation MutableOplogEntry::makeUpdateOperation(const NamespaceString nss,
                                                     UUID uuid,
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

ReplOperation MutableOplogEntry::makeCreateCommand(const NamespaceString nss,
                                                   const CollectionOptions& options,
                                                   const BSONObj& idIndex) {

    ReplOperation op;
    op.setOpType(OpTypeEnum::kCommand);
    op.setNss(nss.getCommandNS());
    op.setUuid(options.uuid);
    op.setObject(makeCreateCollCmdObj(nss, options, idIndex));
    return op;
}

ReplOperation MutableOplogEntry::makeCreateIndexesCommand(const NamespaceString nss,
                                                          CollectionUUID uuid,
                                                          const BSONObj& indexDoc) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kCommand);
    op.setNss(nss.getCommandNS());
    op.setUuid(uuid);

    BSONObjBuilder builder;
    builder.append("createIndexes", nss.coll());
    builder.appendElements(indexDoc);

    op.setObject(builder.obj());

    return op;
}

ReplOperation MutableOplogEntry::makeDeleteOperation(const NamespaceString& nss,
                                                     UUID uuid,
                                                     const BSONObj& docToDelete) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kDelete);
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToDelete.getOwned());
    return op;
}

StatusWith<MutableOplogEntry> MutableOplogEntry::parse(const BSONObj& object) {
    try {
        MutableOplogEntry oplogEntry;
        oplogEntry.parseProtected(IDLParserErrorContext("OplogEntryBase"), object);
        return oplogEntry;
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
}

void MutableOplogEntry::setOpTime(const OpTime& opTime) & {
    setTimestamp(opTime.getTimestamp());
    if (opTime.getTerm() != OpTime::kUninitializedTerm)
        setTerm(opTime.getTerm());
}

OpTime MutableOplogEntry::getOpTime() const {
    long long term = OpTime::kUninitializedTerm;
    if (getTerm()) {
        term = getTerm().get();
    }
    return OpTime(getTimestamp(), term);
}

size_t OplogEntry::getDurableReplOperationSize(const DurableReplOperation& op) {
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

OplogEntry::OplogEntry(BSONObj rawInput) : _raw(std::move(rawInput)) {
    _raw = _raw.getOwned();

    parseProtected(IDLParserErrorContext("OplogEntryBase"), _raw);

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
                       const mongo::Date_t& wallClockTime,
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
    return getCommandType() == CommandType::kApplyOps &&
        getObject()[ApplyOpsCommandInfoBase::kPrepareFieldName].booleanSafe();
}

bool OplogEntry::isSingleOplogEntryTransaction() const {
    if (getCommandType() != CommandType::kApplyOps || !getTxnNumber() || !getSessionId() ||
        getObject()[ApplyOpsCommandInfoBase::kPartialTxnFieldName].booleanSafe()) {
        return false;
    }
    auto prevOptimeOpt = getPrevWriteOpTimeInTransaction();
    if (!prevOptimeOpt) {
        // If there is no prevWriteOptime, then this oplog entry is not a part of a transaction.
        return false;
    }
    return prevOptimeOpt->isNull();
}

bool OplogEntry::isEndOfLargeTransaction() const {
    if (getCommandType() != CommandType::kApplyOps) {
        // If the oplog entry is neither commit nor abort, then it must be an applyOps. Otherwise,
        // it cannot be a termainal oplog entry of a large transaction.
        return false;
    }
    auto prevOptimeOpt = getPrevWriteOpTimeInTransaction();
    if (!prevOptimeOpt) {
        // If the oplog entry is neither commit nor abort, then it must be an applyOps. Otherwise,
        // it cannot be a terminal oplog entry of a large transaction.
        return false;
    }
    // There should be a previous oplog entry in a multiple oplog entry transaction if this is
    // supposed to be the last one. The first oplog entry in a large transaction will have a null
    // ts.
    return !prevOptimeOpt->isNull() && !isPartialTransaction();
}

bool OplogEntry::isSingleOplogEntryTransactionWithCommand() const {
    if (!isSingleOplogEntryTransaction()) {
        return false;
    }
    // Since we know that this oplog entry at this point is part of a transaction, we can safely
    // assume that it has an applyOps field.
    auto applyOps = getObject().getField("applyOps");
    // Iterating through the entire applyOps array is not optimal for performance. A potential
    // optimization, if necessary, could be to ensure the primary always constructs applyOps oplog
    // entries with commands at the beginning.
    for (BSONElement e : applyOps.Array()) {
        auto ns = e.Obj().getField("ns");
        if (!ns.eoo() && NamespaceString(ns.String()).isCommand()) {
            return true;
        }
    }
    return false;
}

BSONElement OplogEntry::getIdElement() const {
    invariant(isCrudOpType());
    if (getOpType() == OpTypeEnum::kUpdate) {
        // We cannot use getObjectContainingDocumentKey() here because the BSONObj will go out
        // of scope after we return the BSONElement.
        fassert(31080, getObject2() != boost::none);
        return getObject2()->getField("_id");
    } else {
        return getObject()["_id"];
    }
}

BSONObj OplogEntry::getOperationToApply() const {
    return getObject();
}

BSONObj OplogEntry::getObjectContainingDocumentKey() const {
    invariant(isCrudOpType());
    if (getOpType() == OpTypeEnum::kUpdate) {
        fassert(31081, getObject2() != boost::none);
        return *getObject2();
    } else {
        return getObject();
    }
}

OplogEntry::CommandType OplogEntry::getCommandType() const {
    return _commandType;
}

int OplogEntry::getRawObjSizeBytes() const {
    return _raw.objsize();
}

std::string OplogEntry::toString() const {
    return _raw.toString();
}

std::ostream& operator<<(std::ostream& s, const OplogEntry& o) {
    return s << o.toString();
}

std::ostream& operator<<(std::ostream& s, const ReplOperation& o) {
    return s << o.toBSON().toString();
}

}  // namespace repl
}  // namespace mongo
