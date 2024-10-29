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


#include "mongo/db/repl/oplog_entry.h"

#include <array>
#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

/**
 * Returns a document representing an oplog entry with the given fields.
 */
BSONObj makeOplogEntryDoc(OpTime opTime,
                          OpTypeEnum opType,
                          const NamespaceString& nss,
                          const boost::optional<UUID>& uuid,
                          const boost::optional<bool>& fromMigrate,
                          const boost::optional<bool>& checkExistenceForDiffInsert,
                          int64_t version,
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
                          const boost::optional<repl::RetryImageEnum>& needsRetryImage) {
    BSONObjBuilder builder;
    if (idField) {
        idField->addToBsonObj(&builder, OplogEntryBase::k_idFieldName);
    }
    sessionInfo.serialize(&builder);
    builder.append(OplogEntryBase::kTimestampFieldName, opTime.getTimestamp());
    builder.append(OplogEntryBase::kTermFieldName, opTime.getTerm());
    builder.append(OplogEntryBase::kVersionFieldName, version);
    builder.append(OplogEntryBase::kOpTypeFieldName, OpType_serializer(opType));
    if (nss.tenantId() && gMultitenancySupport &&
        gFeatureFlagRequireTenantID.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        nss.tenantId()->serializeToBSON(OplogEntryBase::kTidFieldName, &builder);
    }
    builder.append(OplogEntryBase::kNssFieldName,
                   NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    builder.append(OplogEntryBase::kWallClockTimeFieldName, wallClockTime);
    if (uuid) {
        uuid->appendToBuilder(&builder, OplogEntryBase::kUuidFieldName);
    }
    if (fromMigrate) {
        builder.append(OplogEntryBase::kFromMigrateFieldName, fromMigrate.value());
    }
    if (checkExistenceForDiffInsert) {
        builder.append(OplogEntryBase::kCheckExistenceForDiffInsertFieldName,
                       checkExistenceForDiffInsert.value());
    }
    builder.append(OplogEntryBase::kObjectFieldName, oField);
    if (o2Field) {
        builder.append(OplogEntryBase::kObject2FieldName, o2Field.value());
    }
    if (isUpsert) {
        invariant(o2Field);
        builder.append(OplogEntryBase::kUpsertFieldName, isUpsert.value());
    }
    if (statementIds.size() == 1) {
        builder.append(OplogEntryBase::kStatementIdsFieldName, statementIds.front());
    } else if (!statementIds.empty()) {
        builder.append(OplogEntryBase::kStatementIdsFieldName, statementIds);
    }
    if (prevWriteOpTimeInTransaction) {
        const BSONObj localObject = prevWriteOpTimeInTransaction.value().toBSON();
        builder.append(OplogEntryBase::kPrevWriteOpTimeInTransactionFieldName, localObject);
    }
    if (preImageOpTime) {
        const BSONObj localObject = preImageOpTime.value().toBSON();
        builder.append(OplogEntryBase::kPreImageOpTimeFieldName, localObject);
    }
    if (postImageOpTime) {
        const BSONObj localObject = postImageOpTime.value().toBSON();
        builder.append(OplogEntryBase::kPostImageOpTimeFieldName, localObject);
    }

    if (destinedRecipient) {
        builder.append(OplogEntryBase::kDestinedRecipientFieldName,
                       destinedRecipient.value().toString());
    }

    if (needsRetryImage) {
        builder.append(OplogEntryBase::kNeedsRetryImageFieldName,
                       RetryImage_serializer(needsRetryImage.value()));
    }
    return builder.obj();
}
}  // namespace

CommandTypeEnum parseCommandType(const BSONObj& objectField) {
    return CommandType_parse(IDLParserContext("commandString"),
                             objectField.firstElementFieldNameStringData());
}

void ReplOperation::extractPrePostImageForTransaction(boost::optional<ImageBundle>* image) const {
    auto needsRetryImage = getNeedsRetryImage();
    if (!needsRetryImage) {
        return;
    }

    uassert(6054001,
            fmt::format("{} can only store the pre or post image of one findAndModify operation "
                        "for each transaction",
                        NamespaceString::kConfigImagesNamespace.toStringForErrorMsg()),
            !(*image));

    switch (*needsRetryImage) {
        case repl::RetryImageEnum::kPreImage: {
            invariant(!getPreImage().isEmpty());
            *image = ImageBundle{repl::RetryImageEnum::kPreImage, getPreImage(), Timestamp{}};
            break;
        }
        case repl::RetryImageEnum::kPostImage: {
            invariant(!getPostImage().isEmpty());
            *image = ImageBundle{repl::RetryImageEnum::kPostImage, getPostImage(), Timestamp{}};
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
}

void ReplOperation::setTid(boost::optional<mongo::TenantId> value) & {
    if (gMultitenancySupport &&
        gFeatureFlagRequireTenantID.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()))
        DurableReplOperation::setTid(value);
}

// Static
ReplOperation MutableOplogEntry::makeInsertOperation(const NamespaceString& nss,
                                                     UUID uuid,
                                                     const BSONObj& docToInsert,
                                                     const BSONObj& docKey) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kInsert);

    op.setTid(nss.tenantId());
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToInsert.getOwned());
    op.setObject2(docKey.getOwned());
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

    op.setTid(nss.tenantId());
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

    op.setTid(nss.tenantId());
    op.setNss(nss.getCommandNS());
    op.setUuid(options.uuid);
    op.setObject(makeCreateCollCmdObj(nss, options, idIndex));
    return op;
}

ReplOperation MutableOplogEntry::makeCreateIndexesCommand(const NamespaceString nss,
                                                          const UUID& uuid,
                                                          const BSONObj& indexDoc) {
    ReplOperation op;
    op.setOpType(OpTypeEnum::kCommand);

    op.setTid(nss.tenantId());
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

    op.setTid(nss.tenantId());
    op.setNss(nss);
    op.setUuid(uuid);
    op.setObject(docToDelete.getOwned());
    return op;
}

StatusWith<MutableOplogEntry> MutableOplogEntry::parse(const BSONObj& object) {
    const auto tid = OplogEntry::parseTid(object);
    try {
        MutableOplogEntry oplogEntry;
        const auto vts = tid
            ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
                  *tid, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
            : boost::none;
        oplogEntry.parseProtected(
            IDLParserContext("OplogEntryBase", vts, tid, SerializationContext::stateDefault()),
            object);
        return oplogEntry;
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
}

ReplOperation MutableOplogEntry::toReplOperation() const noexcept {
    return ReplOperation(getDurableReplOperation());
}

void MutableOplogEntry::setTid(boost::optional<mongo::TenantId> value) & {
    // Only set Tid if we have a TenantId value and the server parameter and feature flag are on.
    if (value && gMultitenancySupport &&
        gFeatureFlagRequireTenantID.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()))
        getDurableReplOperation().setTid(std::move(value));
}

void MutableOplogEntry::setOpTime(const OpTime& opTime) & {
    setTimestamp(opTime.getTimestamp());
    if (opTime.getTerm() != OpTime::kUninitializedTerm)
        setTerm(opTime.getTerm());
}

OpTime MutableOplogEntry::getOpTime() const {
    long long term = OpTime::kUninitializedTerm;
    if (getTerm()) {
        term = getTerm().value();
    }
    return OpTime(getTimestamp(), term);
}

size_t DurableOplogEntry::getDurableReplOperationSize(const DurableReplOperation& op) {
    const auto& stmtIds = op.getStatementIds();
    return sizeof(op) + (op.getTid() ? op.getTid()->toString().size() : 0) + op.getNss().size() +
        op.getObject().objsize() + (op.getObject2() ? op.getObject2()->objsize() : 0) +
        (sizeof(std::vector<StmtId>) + (sizeof(StmtId) * stmtIds.size()));
}

StatusWith<DurableOplogEntry> DurableOplogEntry::parse(const BSONObj& object) {
    try {
        return DurableOplogEntry(object);
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
}

DurableOplogEntry::DurableOplogEntry(BSONObj rawInput) : _raw(std::move(rawInput)) {
    _raw = _raw.getOwned();

    const auto tid = OplogEntry::parseTid(_raw);

    const auto vts = tid
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tid, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    parseProtected(
        IDLParserContext("OplogEntryBase", vts, tid, SerializationContext::stateDefault()), _raw);

    // Parse command type from 'o' and 'o2' fields.
    if (isCommand()) {
        _commandType = parseCommandType(getObject());
    }
}

DurableOplogEntry::DurableOplogEntry(OpTime opTime,
                                     OpTypeEnum opType,
                                     const NamespaceString& nss,
                                     const boost::optional<UUID>& uuid,
                                     const boost::optional<bool>& fromMigrate,
                                     const boost::optional<bool>& checkExistenceForDiffInsert,
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
                                     const boost::optional<repl::RetryImageEnum>& needsRetryImage)
    : DurableOplogEntry(makeOplogEntryDoc(opTime,
                                          opType,
                                          nss,
                                          uuid,
                                          fromMigrate,
                                          checkExistenceForDiffInsert,
                                          version,
                                          oField,
                                          o2Field,
                                          sessionInfo,
                                          isUpsert,
                                          wallClockTime,
                                          statementIds,
                                          prevWriteOpTimeInTransaction,
                                          preImageOpTime,
                                          postImageOpTime,
                                          destinedRecipient,
                                          idField,
                                          needsRetryImage)) {}

bool DurableOplogEntry::isCommand() const {
    return getOpType() == OpTypeEnum::kCommand;
}

// static
bool DurableOplogEntry::isCrudOpType(OpTypeEnum opType) {
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

bool DurableOplogEntry::isCrudOpType() const {
    return isCrudOpType(getOpType());
}

bool DurableOplogEntry::isUpdateOrDelete() const {
    auto opType = getOpType();
    switch (opType) {
        case OpTypeEnum::kDelete:
        case OpTypeEnum::kUpdate:
            return true;
        case OpTypeEnum::kInsert:
        case OpTypeEnum::kCommand:
        case OpTypeEnum::kNoop:
            return false;
    }
    MONGO_UNREACHABLE;
}

bool DurableOplogEntry::shouldPrepare() const {
    return getCommandType() == CommandTypeEnum::kApplyOps &&
        getObject()[ApplyOpsCommandInfoBase::kPrepareFieldName].booleanSafe();
}

bool DurableOplogEntry::applyOpsIsLinkedTransactionally() const {
    // An applyOps with a prevWriteOpTime is part of a transaction, unless multiOpType is
    // kApplyOpsAppliedSeparately.
    return bool(getPrevWriteOpTimeInTransaction()) &&
        getMultiOpType().value_or(MultiOplogEntryType::kLegacyMultiOpType) !=
        MultiOplogEntryType::kApplyOpsAppliedSeparately;
}

bool DurableOplogEntry::isInTransaction() const {
    if (getCommandType() == CommandTypeEnum::kAbortTransaction ||
        getCommandType() == CommandTypeEnum::kCommitTransaction)
        return true;
    if (!getTxnNumber() || !getSessionId())
        return false;
    if (getCommandType() != CommandTypeEnum::kApplyOps)
        return false;
    return applyOpsIsLinkedTransactionally();
}

bool DurableOplogEntry::isSingleOplogEntryTransaction() const {
    if (getCommandType() != CommandTypeEnum::kApplyOps || !getTxnNumber() || !getSessionId() ||
        getObject()[ApplyOpsCommandInfoBase::kPartialTxnFieldName].booleanSafe()) {
        return false;
    }
    auto prevOptimeOpt = getPrevWriteOpTimeInTransaction();
    if (!prevOptimeOpt ||
        getMultiOpType().value_or(MultiOplogEntryType::kLegacyMultiOpType) ==
            MultiOplogEntryType::kApplyOpsAppliedSeparately) {
        // If there is no prevWriteOptime, then this oplog entry is not a part of a transaction.
        return false;
    }
    return prevOptimeOpt->isNull();
}

bool DurableOplogEntry::isEndOfLargeTransaction() const {
    if (getCommandType() != CommandTypeEnum::kApplyOps) {
        // If the oplog entry is neither commit nor abort, then it must be an applyOps. Otherwise,
        // it cannot be a terminal oplog entry of a large transaction.
        return false;
    }
    auto prevOptimeOpt = getPrevWriteOpTimeInTransaction();
    if (!prevOptimeOpt) {
        // If there is no prevWriteOptime, then this oplog entry is not a part of a transaction.
        return false;
    }
    // There should be a previous oplog entry in a multiple oplog entry transaction if this is
    // supposed to be the last one. The first oplog entry in a large transaction will have a null
    // ts.  The end of a large transaction should not have a partialTxn field, nor should
    // multiOpType be set to kApplyOpsAppliedSeparately
    return !prevOptimeOpt->isNull() && !isPartialTransaction() &&
        getMultiOpType().value_or(MultiOplogEntryType::kLegacyMultiOpType) !=
        MultiOplogEntryType::kApplyOpsAppliedSeparately;
}

bool DurableOplogEntry::isSingleOplogEntryTransactionWithCommand() const {
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
        auto const opType = e.Obj().getStringField(OplogEntry::kOpTypeFieldName);
        if (opType == "c"_sd) {
            return true;
        }
    }
    return false;
}

bool DurableOplogEntry::isIndexCommandType() const {
    return getOpType() == OpTypeEnum::kCommand &&
        ((getCommandType() == CommandTypeEnum::kCreateIndexes) ||
         (getCommandType() == CommandTypeEnum::kStartIndexBuild) ||
         (getCommandType() == CommandTypeEnum::kCommitIndexBuild) ||
         (getCommandType() == CommandTypeEnum::kAbortIndexBuild) ||
         (getCommandType() == CommandTypeEnum::kDropIndexes));
}

BSONElement DurableOplogEntry::getIdElement() const {
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

BSONObj DurableOplogEntry::getOperationToApply() const {
    return getObject();
}

BSONObj DurableOplogEntry::getObjectContainingDocumentKey() const {
    invariant(isCrudOpType());
    if (getOpType() == OpTypeEnum::kUpdate) {
        fassert(31081, getObject2() != boost::none);
        return *getObject2();
    } else {
        return getObject();
    }
}

CommandTypeEnum DurableOplogEntry::getCommandType() const {
    return _commandType;
}

int DurableOplogEntry::getRawObjSizeBytes() const {
    return _raw.objsize();
}

std::string DurableOplogEntry::toString() const {
    return _raw.toString();
}

std::ostream& operator<<(std::ostream& s, const DurableOplogEntry& o) {
    return s << o.toString();
}

std::ostream& operator<<(std::ostream& s, const OplogEntry& o) {
    return s << o.toStringForLogging();
}

std::ostream& operator<<(std::ostream& s, const ReplOperation& o) {
    return s << o.toBSON().toString();
}

OplogEntry::OplogEntry(DurableOplogEntry entry)
    : _entry(std::move(entry)), _needsRetryImage(_entry.getNeedsRetryImage()) {}

OplogEntry::OplogEntry(const BSONObj& entry)
    : OplogEntry(uassertStatusOK(DurableOplogEntry::parse(entry))) {}
void OplogEntry::setEntry(DurableOplogEntry entry) {
    _entry = std::move(entry);
}

bool operator==(const OplogEntry& lhs, const OplogEntry& rhs) {
    if (lhs.isForCappedCollection() != rhs.isForCappedCollection()) {
        return false;
    }

    return lhs.getEntry() == rhs.getEntry();
}

StatusWith<OplogEntry> OplogEntry::parse(const BSONObj& object) {
    auto parseStatus = DurableOplogEntry::parse(object);

    if (!parseStatus.isOK()) {
        return parseStatus;
    }

    return OplogEntry(std::move(parseStatus.getValue()));
}

boost::optional<TenantId> OplogEntry::parseTid(const BSONObj& object) {
    if (!gMultitenancySupport) {
        return boost::none;
    }
    BSONElement tidElem = object["tid"];
    if (tidElem.eoo()) {
        return boost::none;
    }
    return TenantId::parseFromBSON(tidElem);
}

std::string OplogEntry::toStringForLogging() const {
    return toBSONForLogging().toString();
}
BSONObj OplogEntry::toBSONForLogging() const {
    BSONObjBuilder builder;
    auto entry = _entry.toBSON();

    builder.append("oplogEntry", entry);

    if (_isForCappedCollection) {
        builder.append("isForCappedCollection", _isForCappedCollection);
    }

    return builder.obj();
}

bool OplogEntry::isForCappedCollection() const {
    return _isForCappedCollection;
}

void OplogEntry::setIsForCappedCollection(bool isForCappedCollection) {
    _isForCappedCollection = isForCappedCollection;
}

const boost::optional<mongo::Value>& OplogEntry::get_id() const& {
    return _entry.get_id();
}

const std::vector<StmtId>& OplogEntry::getStatementIds() const& {
    return _entry.getStatementIds();
}

const OperationSessionInfo& OplogEntry::getOperationSessionInfo() const {
    return _entry.getOperationSessionInfo();
}
const boost::optional<mongo::LogicalSessionId>& OplogEntry::getSessionId() const {
    return _entry.getSessionId();
}

boost::optional<std::int64_t> OplogEntry::getTxnNumber() const {
    return _entry.getTxnNumber();
}

const DurableReplOperation& OplogEntry::getDurableReplOperation() const {
    return _entry.getDurableReplOperation();
}

mongo::repl::OpTypeEnum OplogEntry::getOpType() const {
    return _entry.getOpType();
}

const boost::optional<mongo::TenantId>& OplogEntry::getTid() const {
    return _entry.getTid();
}

const mongo::NamespaceString& OplogEntry::getNss() const {
    return _entry.getNss();
}

const boost::optional<mongo::UUID>& OplogEntry::getUuid() const {
    return _entry.getUuid();
}

const mongo::BSONObj& OplogEntry::getObject() const {
    return _entry.getObject();
}

const boost::optional<mongo::BSONObj>& OplogEntry::getObject2() const {
    return _entry.getObject2();
}

boost::optional<bool> OplogEntry::getUpsert() const {
    return _entry.getUpsert();
}

const boost::optional<mongo::repl::OpTime>& OplogEntry::getPreImageOpTime() const {
    return _entry.getPreImageOpTime();
}

const boost::optional<mongo::ShardId>& OplogEntry::getDestinedRecipient() const {
    return _entry.getDestinedRecipient();
}

const mongo::Timestamp& OplogEntry::getTimestamp() const {
    return _entry.getTimestamp();
}

boost::optional<std::int64_t> OplogEntry::getTerm() const {
    return _entry.getTerm();
}

const mongo::Date_t& OplogEntry::getWallClockTime() const {
    return _entry.getWallClockTime();
}

std::int64_t OplogEntry::getVersion() const {
    return _entry.getVersion();
}

boost::optional<bool> OplogEntry::getFromMigrate() const& {
    return _entry.getFromMigrate();
}

bool OplogEntry::getCheckExistenceForDiffInsert() const& {
    return _entry.getCheckExistenceForDiffInsert().get_value_or(false);
}

const boost::optional<mongo::UUID>& OplogEntry::getFromTenantMigration() const& {
    return _entry.getFromTenantMigration();
}

const boost::optional<mongo::repl::OpTime>& OplogEntry::getDonorOpTime() const& {
    return _entry.getDonorOpTime();
}

boost::optional<std::int64_t> OplogEntry::getDonorApplyOpsIndex() const& {
    return _entry.getDonorApplyOpsIndex();
}

const boost::optional<mongo::repl::OpTime>& OplogEntry::getPrevWriteOpTimeInTransaction() const& {
    return _entry.getPrevWriteOpTimeInTransaction();
}

const boost::optional<mongo::repl::OpTime>& OplogEntry::getPostImageOpTime() const& {
    return _entry.getPostImageOpTime();
}

boost::optional<mongo::repl::MultiOplogEntryType> OplogEntry::getMultiOpType() const& {
    return _entry.getMultiOpType();
}

boost::optional<RetryImageEnum> OplogEntry::getNeedsRetryImage() const {
    return _needsRetryImage;
}

void OplogEntry::clearNeedsRetryImage() {
    _needsRetryImage = boost::none;
}

OpTime OplogEntry::getOpTime() const {
    return _entry.getOpTime();
}

bool OplogEntry::isCommand() const {
    return _entry.isCommand();
}

bool OplogEntry::applyOpsIsLinkedTransactionally() const {
    return _entry.applyOpsIsLinkedTransactionally();
}

bool OplogEntry::isInTransaction() const {
    return _entry.isInTransaction();
}

bool OplogEntry::isPartialTransaction() const {
    return _entry.isPartialTransaction();
}

bool OplogEntry::isEndOfLargeTransaction() const {
    return _entry.isEndOfLargeTransaction();
}

bool OplogEntry::isPreparedCommit() const {
    return _entry.isPreparedCommit();
}

bool OplogEntry::isPreparedAbort() const {
    return _entry.isPreparedAbort();
}

bool OplogEntry::isPreparedCommitOrAbort() const {
    return _entry.isPreparedCommitOrAbort();
}

bool OplogEntry::isPreparedTransactionCommand() const {
    return _entry.isPreparedTransactionCommand();
}

bool OplogEntry::isTerminalApplyOps() const {
    return _entry.isTerminalApplyOps();
}

bool OplogEntry::isSingleOplogEntryTransaction() const {
    return _entry.isSingleOplogEntryTransaction();
}

bool OplogEntry::isSingleOplogEntryTransactionWithCommand() const {
    return _entry.isSingleOplogEntryTransactionWithCommand();
}

bool OplogEntry::shouldLogAsDDLOperation() const {
    constexpr std::array<std::string_view, 7> ddlOpsToLog{"create",
                                                          "drop",
                                                          "renameCollection",
                                                          "collMod",
                                                          "dropDatabase",
                                                          "createIndexes",
                                                          "dropIndexes"};
    return _entry.isCommand() &&
        std::find(ddlOpsToLog.begin(),
                  ddlOpsToLog.end(),
                  _entry.getObject().firstElementFieldName()) != ddlOpsToLog.end();
}

uint64_t OplogEntry::getApplyOpsIndex() const {
    return _applyOpsIndex;
}

void OplogEntry::setApplyOpsIndex(uint64_t value) {
    _applyOpsIndex = value;
}

const boost::optional<mongo::Timestamp>& OplogEntry::getApplyOpsTimestamp() const {
    return _applyOpsTimestamp;
}

void OplogEntry::setApplyOpsTimestamp(boost::optional<mongo::Timestamp> value) {
    _applyOpsTimestamp = value;
}

const boost::optional<mongo::Date_t>& OplogEntry::getApplyOpsWallClockTime() const {
    return _applyOpsWallClockTime;
}
void OplogEntry::setApplyOpsWallClockTime(boost::optional<mongo::Date_t> value) {
    _applyOpsWallClockTime = value;
}

mongo::Timestamp OplogEntry::getTimestampForPreImage() const {
    return getApplyOpsTimestamp().get_value_or(getTimestamp());
}

mongo::Date_t OplogEntry::getWallClockTimeForPreImage() const {
    return getApplyOpsWallClockTime().get_value_or(getWallClockTime());
}

bool OplogEntry::isCrudOpType() const {
    return _entry.isCrudOpType();
}

bool OplogEntry::isUpdateOrDelete() const {
    return _entry.isUpdateOrDelete();
}

bool OplogEntry::isIndexCommandType() const {
    return _entry.isIndexCommandType();
}

bool OplogEntry::shouldPrepare() const {
    return _entry.shouldPrepare();
}

BSONElement OplogEntry::getIdElement() const {
    return _entry.getIdElement();
}

BSONObj OplogEntry::getOperationToApply() const {
    return _entry.getOperationToApply();
}

BSONObj OplogEntry::getObjectContainingDocumentKey() const {
    return _entry.getObjectContainingDocumentKey();
}

OplogEntry::CommandType OplogEntry::getCommandType() const {
    return _entry.getCommandType();
}

int OplogEntry::getRawObjSizeBytes() const {
    return _entry.getRawObjSizeBytes();
}

OplogEntryParserNonStrict::OplogEntryParserNonStrict(const BSONObj& oplogEntry)
    : _oplogEntryObject{oplogEntry.getOwned()} {}

repl::OpTime OplogEntryParserNonStrict::getOpTime() const {
    return uassertStatusOKWithContext(repl::OpTime::parseFromOplogEntry(_oplogEntryObject),
                                      str::stream() << "Failed to parse opTime");
}

repl::OpTypeEnum OplogEntryParserNonStrict::getOpType() const {
    auto opTypeElement = _oplogEntryObject[repl::OplogEntry::kOpTypeFieldName];
    uassert(8881100,
            str::stream() << "Invalid '" << repl::OplogEntry::kOpTypeFieldName
                          << "' field type (expected String)",
            opTypeElement.type() == BSONType::String);
    return repl::OpType_parse(IDLParserContext("ChangeStreamEntry.op"),
                              opTypeElement.checkAndGetStringData());
}

BSONObj OplogEntryParserNonStrict::getObject() const {
    auto objectElement = _oplogEntryObject[repl::OplogEntry::kObjectFieldName];
    uassert(8881101,
            str::stream() << "Invalid '" << repl::OplogEntry::kObjectFieldName
                          << "' field type (expected Object)",
            objectElement.isABSONObj());
    return objectElement.Obj();
}

}  // namespace repl
}  // namespace mongo
