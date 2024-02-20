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

#include "mongo/db/ops/write_ops.h"

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <utility>
#include <variant>

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/bulk_write_gen.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

using write_ops::DeleteCommandReply;
using write_ops::DeleteCommandRequest;
using write_ops::DeleteOpEntry;
using write_ops::FindAndModifyCommandReply;
using write_ops::InsertCommandReply;
using write_ops::InsertCommandRequest;
using write_ops::UpdateCommandReply;
using write_ops::UpdateCommandRequest;
using write_ops::UpdateOpEntry;
using write_ops::WriteCommandRequestBase;

namespace {

// This constant accounts for the null terminator in each field name and the BSONType byte for
// each element.
static constexpr int kPerElementOverhead = 2;

// This constant accounts for the size of a bool.
static constexpr int kBoolSize = 1;

// This constant tracks the overhead for serializing UUIDs. It includes 1 byte for the
// 'BinDataType', 4 bytes for serializing the integer size of the UUID, and finally, 16 bytes
// for the UUID itself.
static const int kUUIDSize = 21;

// This constant accounts for the size of a 32-bit integer.
static const int kIntSize = 4;

template <class T>
void checkOpCountForCommand(const T& op, size_t numOps) {
    uassert(ErrorCodes::InvalidLength,
            str::stream() << "Write batch sizes must be between 1 and "
                          << write_ops::kMaxWriteBatchSize << ". Got " << numOps << " operations.",
            numOps != 0 && numOps <= write_ops::kMaxWriteBatchSize);

    if (const auto& stmtIds = op.getWriteCommandRequestBase().getStmtIds()) {
        uassert(
            ErrorCodes::InvalidLength,
            str::stream() << "Number of statement ids must match the number of batch entries. Got "
                          << stmtIds->size() << " statement ids but " << numOps
                          << " operations. Statement ids: " << BSON("stmtIds" << *stmtIds)
                          << ". Write command: " << op.toBSON({}),
            stmtIds->size() == numOps);
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "May not specify both stmtId and stmtIds in write command. Got "
                              << BSON("stmtId" << *op.getWriteCommandRequestBase().getStmtId()
                                               << "stmtIds" << *stmtIds)
                              << ". Write command: " << op.toBSON({}),
                !op.getWriteCommandRequestBase().getStmtId());
    }
}

// Utility which estimates the size of 'WriteCommandRequestBase' when serialized.
int getWriteCommandRequestBaseSize(const WriteCommandRequestBase& base) {
    static const int kSizeOfOrderedField =
        write_ops::WriteCommandRequestBase::kOrderedFieldName.size() + kBoolSize +
        kPerElementOverhead;
    static const int kSizeOfBypassDocumentValidationField =
        write_ops::WriteCommandRequestBase::kBypassDocumentValidationFieldName.size() + kBoolSize +
        kPerElementOverhead;

    auto estSize = static_cast<int>(BSONObj::kMinBSONLength) + kSizeOfOrderedField +
        kSizeOfBypassDocumentValidationField;

    if (auto stmtId = base.getStmtId(); stmtId) {
        estSize += write_ops::WriteCommandRequestBase::kStmtIdFieldName.size() +
            write_ops::kStmtIdSize + kPerElementOverhead;
    }

    if (auto stmtIds = base.getStmtIds(); stmtIds) {
        estSize += write_ops::WriteCommandRequestBase::kStmtIdsFieldName.size();
        estSize += static_cast<int>(BSONObj::kMinBSONLength);
        estSize +=
            (write_ops::kStmtIdSize + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes) *
            stmtIds->size();
        estSize += kPerElementOverhead;
    }

    if (auto isTimeseries = base.getIsTimeseriesNamespace(); isTimeseries.has_value()) {
        estSize += write_ops::WriteCommandRequestBase::kIsTimeseriesNamespaceFieldName.size() +
            kBoolSize + kPerElementOverhead;
    }

    if (auto collUUID = base.getCollectionUUID(); collUUID) {
        estSize += write_ops::WriteCommandRequestBase::kCollectionUUIDFieldName.size() + kUUIDSize +
            kPerElementOverhead;
    }

    if (auto encryptionInfo = base.getEncryptionInformation(); encryptionInfo) {
        estSize += write_ops::WriteCommandRequestBase::kEncryptionInformationFieldName.size() +
            encryptionInfo->toBSON().objsize() + kPerElementOverhead;
    }

    if (auto query = base.getOriginalQuery(); query) {
        estSize += write_ops::WriteCommandRequestBase::kOriginalQueryFieldName.size() +
            query->objsize() + kPerElementOverhead;
    }

    if (auto originalCollation = base.getOriginalCollation(); originalCollation) {
        estSize += write_ops::WriteCommandRequestBase::kOriginalCollationFieldName.size() +
            originalCollation->objsize() + kPerElementOverhead;
    }

    return estSize;
}

}  // namespace

namespace write_ops {

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
bool readMultiDeleteProperty(const BSONElement& limitElement) {
    // Using a double to avoid throwing away illegal fractional portion. Don't want to accept 0.5
    // here
    const double limit = limitElement.numberDouble();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The limit field in delete objects must be 0 or 1. Got " << limit,
            limit == 0 || limit == 1);

    return limit == 0;
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void writeMultiDeleteProperty(bool isMulti, StringData fieldName, BSONObjBuilder* builder) {
    builder->append(fieldName, isMulti ? 0 : 1);
}

void opTimeSerializerWithTermCheck(repl::OpTime opTime, StringData fieldName, BSONObjBuilder* bob) {
    if (opTime.getTerm() == repl::OpTime::kUninitializedTerm) {
        bob->append(fieldName, opTime.getTimestamp());
    } else {
        opTime.append(bob, fieldName.toString());
    }
}

repl::OpTime opTimeParser(BSONElement elem) {
    if (elem.type() == BSONType::Object) {
        return repl::OpTime::parse(elem.Obj());
    } else if (elem.type() == BSONType::bsonTimestamp) {
        return repl::OpTime(elem.timestamp(), repl::OpTime::kUninitializedTerm);
    }

    uasserted(ErrorCodes::TypeMismatch,
              str::stream() << "Expected BSON type " << BSONType::Object << " or "
                            << BSONType::bsonTimestamp << ", but found " << elem.type());
}

int32_t getStmtIdForWriteAt(const WriteCommandRequestBase& writeCommandBase, size_t writePos) {
    const auto& stmtIds = writeCommandBase.getStmtIds();

    if (stmtIds) {
        return stmtIds->at(writePos);
    }

    const auto& stmtId = writeCommandBase.getStmtId();
    const int32_t kFirstStmtId = stmtId ? *stmtId : 0;
    return kFirstStmtId + writePos;
}

int estimateRuntimeConstantsSize(const mongo::LegacyRuntimeConstants& constants) {
    int size = write_ops::UpdateCommandRequest::kLegacyRuntimeConstantsFieldName.size() +
        static_cast<int>(BSONObj::kMinBSONLength) + kPerElementOverhead;

    // $$NOW
    size +=
        LegacyRuntimeConstants::kLocalNowFieldName.size() + sizeof(Date_t) + kPerElementOverhead;

    // $$CLUSTER_TIME
    size += LegacyRuntimeConstants::kClusterTimeFieldName.size() + sizeof(Timestamp) +
        kPerElementOverhead;

    // $$JS_SCOPE
    if (const auto& scope = constants.getJsScope(); scope.has_value()) {
        size += LegacyRuntimeConstants::kJsScopeFieldName.size() + scope->objsize() +
            kPerElementOverhead;
    }

    // $$IS_MR
    if (const auto& isMR = constants.getIsMapReduce(); isMR.has_value()) {
        size +=
            LegacyRuntimeConstants::kIsMapReduceFieldName.size() + kBoolSize + kPerElementOverhead;
    }

    // $$USER_ROLES
    if (const auto& userRoles = constants.getUserRoles(); userRoles.has_value()) {
        size += LegacyRuntimeConstants::kUserRolesFieldName.size() + userRoles->objsize() +
            kPerElementOverhead;
    }
    return size;
}

int getArrayFiltersFieldSize(const std::vector<mongo::BSONObj>& arrayFilters,
                             const StringData arrayFiltersFieldName) {
    auto size = BSONObj::kMinBSONLength + arrayFiltersFieldName.size() + kPerElementOverhead;
    for (auto&& filter : arrayFilters) {
        // For each filter, we not only need to account for the size of the filter itself,
        // but also for the per array element overhead.
        size += filter.objsize();
        size += write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
    }
    return size;
}

int getUpdateSizeEstimate(const BSONObj& q,
                          const write_ops::UpdateModification& u,
                          const boost::optional<mongo::BSONObj>& c,
                          const bool includeUpsertSupplied,
                          const boost::optional<mongo::BSONObj>& collation,
                          const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters,
                          const boost::optional<mongo::BSONObj>& sort,
                          const mongo::BSONObj& hint,
                          const boost::optional<UUID>& sampleId,
                          const bool includeAllowShardKeyUpdatesWithoutFullShardKeyInQuery) {
    using UpdateOpEntry = write_ops::UpdateOpEntry;
    int estSize = static_cast<int>(BSONObj::kMinBSONLength);

    // Add the sizes of the 'multi' and 'upsert' fields.
    estSize += UpdateOpEntry::kUpsertFieldName.size() + kBoolSize + kPerElementOverhead;
    estSize += UpdateOpEntry::kMultiFieldName.size() + kBoolSize + kPerElementOverhead;

    // Add the size of 'upsertSupplied' field if present.
    if (includeUpsertSupplied) {
        estSize += UpdateOpEntry::kUpsertSuppliedFieldName.size() + kBoolSize + kPerElementOverhead;
    }

    // Add the sizes of the 'q' and 'u' fields.
    estSize += (UpdateOpEntry::kQFieldName.size() + q.objsize() + kPerElementOverhead +
                UpdateOpEntry::kUFieldName.size() + u.objsize() + kPerElementOverhead);

    // Add the size of the 'c' field, if present.
    if (c) {
        estSize += (UpdateOpEntry::kCFieldName.size() + c->objsize() + kPerElementOverhead);
    }

    // Add the size of the 'collation' field, if present.
    if (collation) {
        estSize += (UpdateOpEntry::kCollationFieldName.size() + collation->objsize() +
                    kPerElementOverhead);
    }

    // Add the size of the 'arrayFilters' field, if present.
    if (arrayFilters) {
        estSize +=
            getArrayFiltersFieldSize(arrayFilters.get(), UpdateOpEntry::kArrayFiltersFieldName);
    }

    // Add the size of the 'sort' field, if present.
    if (sort) {
        estSize += UpdateOpEntry::kSortFieldName.size() + sort->objsize() + kPerElementOverhead;
    }

    // Add the size of the 'hint' field, if present.
    if (!hint.isEmpty()) {
        estSize += UpdateOpEntry::kHintFieldName.size() + hint.objsize() + kPerElementOverhead;
    }

    // Add the size of the 'sampleId' field, if present.
    if (sampleId) {
        estSize += UpdateOpEntry::kSampleIdFieldName.size() + kUUIDSize + kPerElementOverhead;
    }

    // Add the size of the '$_allowShardKeyUpdatesWithoutFullShardKeyInQuery' field, if present.
    if (includeAllowShardKeyUpdatesWithoutFullShardKeyInQuery) {
        estSize += UpdateOpEntry::kAllowShardKeyUpdatesWithoutFullShardKeyInQueryFieldName.size() +
            kBoolSize + kPerElementOverhead;
    }

    return estSize;
}

/*
 * Helper function to calculate the estimated size of an update operation in a bulkWrite command.
 *
 * Note: This helper function doesn't currently account for the size needed for the internal field
 * '_allowShardKeyUpdatesWithoutFullShardKeyInQuery' for updateOne without shard key. This should be
 * safe for now because each update operation without shard key is always executed in its own child
 * batch. See `targetWriteOps` for more details.
 * (This needs to change once we start batching multiple updateOne operations without shard key in
 * the same batch.)
 *
 *  TODO (SERVER-82382): Fix sampleId size calculation.
 */
int getBulkWriteUpdateSizeEstimate(const BSONObj& filter,
                                   const write_ops::UpdateModification& updateMods,
                                   const boost::optional<mongo::BSONObj>& constants,
                                   const bool includeUpsertSupplied,
                                   const boost::optional<mongo::BSONObj>& collation,
                                   const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters,
                                   const boost::optional<mongo::BSONObj>& sort,
                                   const BSONObj& hint,
                                   const boost::optional<UUID>& sampleId) {
    int estSize = static_cast<int>(BSONObj::kMinBSONLength);

    // Adds the size of the 'update' field which contains the index of the corresponding namespace.
    estSize += BulkWriteUpdateOp::kUpdateFieldName.size() + kIntSize + kPerElementOverhead;

    // Add the sizes of the 'multi' and 'upsert' fields.
    estSize += BulkWriteUpdateOp::kUpsertFieldName.size() + kBoolSize + kPerElementOverhead;
    estSize += BulkWriteUpdateOp::kMultiFieldName.size() + kBoolSize + kPerElementOverhead;

    // Add the size of 'upsertSupplied' field if present.
    if (includeUpsertSupplied) {
        estSize +=
            BulkWriteUpdateOp::kUpsertSuppliedFieldName.size() + kBoolSize + kPerElementOverhead;
    }

    // Add the sizes of the 'filter' and 'updateMods' fields.
    estSize += (BulkWriteUpdateOp::kFilterFieldName.size() + filter.objsize() +
                kPerElementOverhead + BulkWriteUpdateOp::kUpdateModsFieldName.size() +
                updateMods.objsize() + kPerElementOverhead);

    // Add the size of the 'constants' field, if present.
    if (constants) {
        estSize += (BulkWriteUpdateOp::kConstantsFieldName.size() + constants->objsize() +
                    kPerElementOverhead);
    }

    // Add the size of the 'collation' field, if present.
    if (collation) {
        estSize += (BulkWriteUpdateOp::kCollationFieldName.size() + collation->objsize() +
                    kPerElementOverhead);
    }

    // Add the size of the 'arrayFilters' field, if present.
    if (arrayFilters) {
        estSize +=
            getArrayFiltersFieldSize(arrayFilters.get(), BulkWriteUpdateOp::kArrayFiltersFieldName);
    }

    // Add the size of the 'sort' field, if present.
    if (sort) {
        estSize += BulkWriteUpdateOp::kSortFieldName.size() + sort->objsize() + kPerElementOverhead;
    }

    // Add the size of the 'hint' field, if present.
    if (!hint.isEmpty()) {
        estSize += BulkWriteUpdateOp::kHintFieldName.size() + hint.objsize() + kPerElementOverhead;
    }

    // Add the size of the 'sampleId' field.
    estSize += BulkWriteUpdateOp::kSampleIdFieldName.size() + kUUIDSize + kPerElementOverhead;

    return estSize;
}

int getDeleteSizeEstimate(const BSONObj& q,
                          const boost::optional<mongo::BSONObj>& collation,
                          const mongo::BSONObj& hint,
                          const boost::optional<UUID>& sampleId) {
    using DeleteOpEntry = write_ops::DeleteOpEntry;
    int estSize = static_cast<int>(BSONObj::kMinBSONLength);

    // Add the size of the 'q' field.
    estSize += DeleteOpEntry::kQFieldName.size() + q.objsize() + kPerElementOverhead;

    // Add the size of the 'collation' field, if present.
    if (collation) {
        estSize +=
            DeleteOpEntry::kCollationFieldName.size() + collation->objsize() + kPerElementOverhead;
    }

    // Add the size of the 'limit' field.
    estSize += DeleteOpEntry::kMultiFieldName.size() + kIntSize + kPerElementOverhead;

    // Add the size of the 'hint' field, if present.
    if (!hint.isEmpty()) {
        estSize += DeleteOpEntry::kHintFieldName.size() + hint.objsize() + kPerElementOverhead;
    }

    // Add the size of the 'sampleId' field, if present.
    if (sampleId) {
        estSize += DeleteOpEntry::kSampleIdFieldName.size() + kUUIDSize + kPerElementOverhead;
    }

    return estSize;
}

// TODO (SERVER-82382): Fix sampleId size calculation.
int getBulkWriteDeleteSizeEstimate(const BSONObj& filter,
                                   const boost::optional<mongo::BSONObj>& collation,
                                   const mongo::BSONObj& hint,
                                   const boost::optional<UUID>& sampleId) {
    int estSize = static_cast<int>(BSONObj::kMinBSONLength);

    // Adds the size of the 'delete' field which contains the index of the corresponding namespace.
    estSize += BulkWriteDeleteOp::kDeleteCommandFieldName.size() + kIntSize + kPerElementOverhead;

    // Add the size of the 'filter' field.
    estSize += BulkWriteDeleteOp::kFilterFieldName.size() + filter.objsize() + kPerElementOverhead;

    // Add the size of the 'multi' field.
    estSize += BulkWriteDeleteOp::kMultiFieldName.size() + kBoolSize + kPerElementOverhead;

    // Add the size of the 'collation' field, if present.
    if (collation) {
        estSize += BulkWriteDeleteOp::kCollationFieldName.size() + collation->objsize() +
            kPerElementOverhead;
    }

    // Add the size of the 'hint' field, if present.
    if (!hint.isEmpty()) {
        estSize +=
            (BulkWriteDeleteOp::kHintFieldName.size() + hint.objsize() + kPerElementOverhead);
    }

    // Add the size of the 'sampleId' field.
    estSize += BulkWriteUpdateOp::kSampleIdFieldName.size() + kUUIDSize + kPerElementOverhead;

    return estSize;
}

int getBulkWriteInsertSizeEstimate(const mongo::BSONObj& document) {
    int estSize = static_cast<int>(BSONObj::kMinBSONLength);

    // Adds the size of the 'insert' field which contains the index of the corresponding namespace.
    estSize += BulkWriteInsertOp::kInsertFieldName.size() + kIntSize + kPerElementOverhead;

    // Add the size of the 'document' field.
    estSize +=
        BulkWriteInsertOp::kDocumentFieldName.size() + document.objsize() + kPerElementOverhead;

    return estSize;
}

bool verifySizeEstimate(const write_ops::UpdateOpEntry& update) {
    return write_ops::getUpdateSizeEstimate(
               update.getQ(),
               update.getU(),
               update.getC(),
               update.getUpsertSupplied().has_value(),
               update.getCollation(),
               update.getArrayFilters(),
               update.getSort(),
               update.getHint(),
               update.getSampleId(),
               update.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery().has_value()) >=
        update.toBSON().objsize();
}

bool verifySizeEstimate(const InsertCommandRequest& insertReq,
                        const OpMsgRequest* unparsedRequest) {
    int size = getInsertHeaderSizeEstimate(insertReq);
    for (auto&& docToInsert : insertReq.getDocuments()) {
        size += docToInsert.objsize() + kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    // Return true if 'insertReq' originated from a document sequence and our size estimate exceeds
    // the size limit.
    if (unparsedRequest && !unparsedRequest->sequences.empty() && size > BSONObjMaxUserSize) {
        return true;
    }
    return size >= insertReq.toBSON({} /* commandPassthroughFields */).objsize();
}

bool verifySizeEstimate(const UpdateCommandRequest& updateReq,
                        const OpMsgRequest* unparsedRequest) {
    int size = getUpdateHeaderSizeEstimate(updateReq);

    for (auto&& update : updateReq.getUpdates()) {
        size += getUpdateSizeEstimate(
                    update.getQ(),
                    update.getU(),
                    update.getC(),
                    update.getUpsertSupplied().has_value(),
                    update.getCollation(),
                    update.getArrayFilters(),
                    update.getSort(),
                    update.getHint(),
                    update.getSampleId(),
                    update.getAllowShardKeyUpdatesWithoutFullShardKeyInQuery().has_value()) +
            kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    // Return true if 'updateReq' originated from a document sequence and our size estimate exceeds
    // the size limit.
    if (unparsedRequest && !unparsedRequest->sequences.empty() && size > BSONObjMaxUserSize) {
        return true;
    }
    return size >= updateReq.toBSON({} /* commandPassthroughFields */).objsize();
}

bool verifySizeEstimate(const DeleteCommandRequest& deleteReq,
                        const OpMsgRequest* unparsedRequest) {
    int size = getDeleteHeaderSizeEstimate(deleteReq);

    for (auto&& deleteOp : deleteReq.getDeletes()) {
        size += write_ops::getDeleteSizeEstimate(deleteOp.getQ(),
                                                 deleteOp.getCollation(),
                                                 deleteOp.getHint(),
                                                 deleteOp.getSampleId()) +
            kWriteCommandBSONArrayPerElementOverheadBytes;
    }

    // Return true if 'deleteReq' originated from a document sequence and our size estimate exceeds
    // the size limit.
    if (unparsedRequest && !unparsedRequest->sequences.empty() && size > BSONObjMaxUserSize) {
        return true;
    }
    return size >= deleteReq.toBSON({} /* commandPassthroughFields */).objsize();
}

int getInsertHeaderSizeEstimate(const InsertCommandRequest& insertReq) {
    int size = getWriteCommandRequestBaseSize(insertReq.getWriteCommandRequestBase()) +
        write_ops::InsertCommandRequest::kDocumentsFieldName.size() + kPerElementOverhead +
        static_cast<int>(BSONObj::kMinBSONLength);

    size += InsertCommandRequest::kCommandName.size() + kPerElementOverhead +
        insertReq.getNamespace().size() + 1 /* ns string null terminator */;

    // Handle $tenant. Note that $tenant is injected as a hidden field into all IDL commands, unlike
    // other passthrough fields.
    if (auto tenant = insertReq.getDollarTenant(); tenant.has_value()) {
        size += InsertCommandRequest::kDollarTenantFieldName.size() + OID::kOIDSize +
            kPerElementOverhead;
    }
    return size;
}

int getUpdateHeaderSizeEstimate(const UpdateCommandRequest& updateReq) {
    int size = getWriteCommandRequestBaseSize(updateReq.getWriteCommandRequestBase());

    size += UpdateCommandRequest::kCommandName.size() + kPerElementOverhead +
        updateReq.getNamespace().size() + 1 /* ns string null terminator */;

    size += write_ops::UpdateCommandRequest::kUpdatesFieldName.size() + kPerElementOverhead +
        static_cast<int>(BSONObj::kMinBSONLength);

    // Handle $tenant. Note that $tenant is injected as a hidden field into all IDL commands, unlike
    // other passthrough fields.
    if (auto tenant = updateReq.getDollarTenant(); tenant.has_value()) {
        size += UpdateCommandRequest::kDollarTenantFieldName.size() + OID::kOIDSize +
            kPerElementOverhead;
    }

    // Handle legacy runtime constants.
    if (auto runtimeConstants = updateReq.getLegacyRuntimeConstants();
        runtimeConstants.has_value()) {
        size += estimateRuntimeConstantsSize(*runtimeConstants);
    }

    // Handle let parameters.
    if (auto let = updateReq.getLet(); let.has_value()) {
        size += write_ops::UpdateCommandRequest::kLetFieldName.size() + let->objsize() +
            kPerElementOverhead;
    }
    return size;
}

int getDeleteHeaderSizeEstimate(const DeleteCommandRequest& deleteReq) {
    int size = getWriteCommandRequestBaseSize(deleteReq.getWriteCommandRequestBase());

    size += DeleteCommandRequest::kCommandName.size() + kPerElementOverhead +
        deleteReq.getNamespace().size() + 1 /* ns string null terminator */;

    size += write_ops::DeleteCommandRequest::kDeletesFieldName.size() + kPerElementOverhead +
        static_cast<int>(BSONObj::kMinBSONLength);

    // Handle $tenant. Note that $tenant is injected as a hidden field into all IDL commands, unlike
    // other passthrough fields.
    if (auto tenant = deleteReq.getDollarTenant(); tenant.has_value()) {
        size += DeleteCommandRequest::kDollarTenantFieldName.size() + OID::kOIDSize +
            kPerElementOverhead;
    }

    // Handle legacy runtime constants.
    if (auto runtimeConstants = deleteReq.getLegacyRuntimeConstants();
        runtimeConstants.has_value()) {
        size += estimateRuntimeConstantsSize(*runtimeConstants);
    }

    // Handle let parameters.
    if (auto let = deleteReq.getLet(); let.has_value()) {
        size += write_ops::UpdateCommandRequest::kLetFieldName.size() + let->objsize() +
            kPerElementOverhead;
    }
    return size;
}

bool verifySizeEstimate(const write_ops::DeleteOpEntry& deleteOp) {
    return write_ops::getDeleteSizeEstimate(deleteOp.getQ(),
                                            deleteOp.getCollation(),
                                            deleteOp.getHint(),
                                            deleteOp.getSampleId()) >= deleteOp.toBSON().objsize();
}

bool isClassicalUpdateReplacement(const BSONObj& update) {
    // An empty update object will be treated as replacement as firstElementFieldName() returns "".
    return update.firstElementFieldName()[0] != '$';
}

void checkWriteErrors(const WriteCommandReplyBase& reply) {
    if (!reply.getWriteErrors())
        return;

    const auto& writeErrors = *reply.getWriteErrors();
    uassert(633310, "Write errors must not be empty", !writeErrors.empty());

    const auto& firstError = writeErrors.front();
    uassertStatusOK(firstError.getStatus());
}

UpdateModification UpdateModification::parseFromOplogEntry(const BSONObj& oField,
                                                           const DiffOptions& options) {
    BSONElement vField = oField[kUpdateOplogEntryVersionFieldName];
    BSONElement idField = oField["_id"];

    // If _id field is present, we're getting a replacement style update in which $v can be a user
    // field. Otherwise, $v field has to be $v:2.
    uassert(4772600,
            str::stream() << "Expected _id field or $v:2, but got: " << vField,
            idField.ok() ||
                (vField.ok() &&
                 vField.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kDeltaV2)));

    // It is important to check for '_id' field first, because a replacement style update can still
    // have a '$v' field in the object.
    if (!idField.ok()) {
        // Make sure there's a diff field.
        BSONElement diff = oField[update_oplog_entry::kDiffObjectFieldName];
        uassert(4772601,
                str::stream() << "Expected 'diff' field to be an object, instead got type: "
                              << diff.type(),
                diff.type() == BSONType::Object);

        return UpdateModification(doc_diff::Diff{diff.embeddedObject()}, DeltaTag{}, options);
    } else {
        // Treat it as a a full replacement update.
        return UpdateModification(oField, ReplacementTag{});
    }
}

UpdateModification::UpdateModification(doc_diff::Diff diff, DeltaTag, DiffOptions options)
    : _update(DeltaUpdate{std::move(diff), options}) {}

UpdateModification::UpdateModification(TransformFunc transform)
    : _update(TransformUpdate{std::move(transform)}) {}

UpdateModification::UpdateModification(BSONElement update) {
    const auto type = update.type();
    if (type == BSONType::Object) {
        _update = UpdateModification(update.Obj())._update;
        return;
    }

    uassert(ErrorCodes::FailedToParse,
            "Update argument must be either an object or an array",
            type == BSONType::Array);

    _update = PipelineUpdate{parsePipelineFromBSON(update)};
}

UpdateModification::UpdateModification(const BSONObj& update) {
    if (isClassicalUpdateReplacement(update)) {
        _update = ReplacementUpdate{update};
    } else {
        _update = ModifierUpdate{update};
    }
}

UpdateModification::UpdateModification(const BSONObj& update, ModifierUpdateTag)
    : _update{ModifierUpdate{update}} {}
UpdateModification::UpdateModification(const BSONObj& update, ReplacementTag)
    : _update{ReplacementUpdate{update}} {}


UpdateModification::UpdateModification(std::vector<BSONObj> pipeline)
    : _update{PipelineUpdate{std::move(pipeline)}} {}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
UpdateModification UpdateModification::parseFromBSON(BSONElement elem) {
    return UpdateModification(elem);
}

int UpdateModification::objsize() const {
    return visit(
        OverloadedVisitor{
            [](const ReplacementUpdate& replacement) -> int { return replacement.bson.objsize(); },
            [](const ModifierUpdate& modifier) -> int { return modifier.bson.objsize(); },
            [](const PipelineUpdate& pipeline) -> int {
                int size = 0;
                std::for_each(pipeline.begin(), pipeline.end(), [&size](const BSONObj& obj) {
                    size += obj.objsize() + kWriteCommandBSONArrayPerElementOverheadBytes;
                });

                return size + kWriteCommandBSONArrayPerElementOverheadBytes;
            },
            [](const DeltaUpdate& delta) -> int { return delta.diff.objsize(); },
            [](const TransformUpdate& transform) -> int {
                return 0;
            }},
        _update);
}

UpdateModification::Type UpdateModification::type() const {
    return visit(
        OverloadedVisitor{[](const ReplacementUpdate& replacement) { return Type::kReplacement; },
                          [](const ModifierUpdate& modifier) { return Type::kModifier; },
                          [](const PipelineUpdate& pipelineUpdate) { return Type::kPipeline; },
                          [](const DeltaUpdate& delta) { return Type::kDelta; },
                          [](const TransformUpdate& transform) {
                              return Type::kTransform;
                          }},
        _update);
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void UpdateModification::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {

    visit(OverloadedVisitor{
              [fieldName, bob](const ReplacementUpdate& replacement) {
                  *bob << fieldName << replacement.bson;
              },
              [fieldName, bob](const ModifierUpdate& modifier) {
                  *bob << fieldName << modifier.bson;
              },
              [fieldName, bob](const PipelineUpdate& pipeline) {
                  BSONArrayBuilder arrayBuilder(bob->subarrayStart(fieldName));
                  for (auto&& stage : pipeline) {
                      arrayBuilder << stage;
                  }
                  arrayBuilder.doneFast();
              },
              [fieldName, bob](const DeltaUpdate& delta) { *bob << fieldName << delta.diff; },
              [](const TransformUpdate& transform) {
              }},
          _update);
}

WriteError::WriteError(int32_t index, Status status) : _index(index), _status(std::move(status)) {}

WriteError WriteError::parse(const BSONObj& obj) {
    auto index = int32_t(obj[WriteError::kIndexFieldName].Int());
    auto status = [&] {
        auto code = ErrorCodes::Error(obj[WriteError::kCodeFieldName].Int());
        auto errmsg = obj[WriteError::kErrmsgFieldName].valueStringDataSafe();

        return Status(code, errmsg, obj);
    }();

    return WriteError(index, std::move(status));
}

BSONObj WriteError::serialize() const {
    BSONObjBuilder errBuilder;
    errBuilder.append(WriteError::kIndexFieldName, _index);

    errBuilder.append(WriteError::kCodeFieldName, int32_t(_status.code()));
    errBuilder.append(WriteError::kErrmsgFieldName, _status.reason());
    if (auto extraInfo = _status.extraInfo()) {
        extraInfo->serialize(&errBuilder);
    }

    return errBuilder.obj();
}

}  // namespace write_ops

InsertCommandRequest InsertOp::parse(const OpMsgRequest& request) {
    auto insertOp = InsertCommandRequest::parse(IDLParserContext("insert"), request);

    validate(insertOp);
    return insertOp;
}

InsertCommandRequest InsertOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    // Passing boost::none since this is legacy code and should not be running in serverless.
    InsertCommandRequest op(NamespaceStringUtil::deserialize(
        boost::none, msg.getns(), SerializationContext::stateDefault()));

    {
        WriteCommandRequestBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(!(msg.reservedField() & InsertOption_ContinueOnError));
        op.setWriteCommandRequestBase(std::move(writeCommandBase));
    }

    uassert(ErrorCodes::InvalidLength, "Need at least one object to insert", msg.moreJSObjs());

    op.setDocuments([&] {
        std::vector<BSONObj> documents;
        while (msg.moreJSObjs()) {
            documents.push_back(msg.nextJsObj());
        }

        return documents;
    }());

    validate(op);
    return op;
}

InsertCommandReply InsertOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));
    return InsertCommandReply::parse(IDLParserContext("insertReply"), obj);
}

void InsertOp::validate(const InsertCommandRequest& insertOp) {
    const auto& docs = insertOp.getDocuments();
    checkOpCountForCommand(insertOp, docs.size());
}

UpdateCommandRequest UpdateOp::parse(const OpMsgRequest& request) {
    auto updateOp = UpdateCommandRequest::parse(IDLParserContext("update"), request);

    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
    return updateOp;
}

UpdateCommandReply UpdateOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return UpdateCommandReply::parse(IDLParserContext("updateReply"), obj);
}

void UpdateOp::validate(const UpdateCommandRequest& updateOp) {
    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
}

FindAndModifyCommandReply FindAndModifyOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return FindAndModifyCommandReply::parse(IDLParserContext("findAndModifyReply"), obj);
}

DeleteCommandRequest DeleteOp::parse(const OpMsgRequest& request) {
    auto deleteOp = DeleteCommandRequest::parse(IDLParserContext("delete"), request);

    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
    return deleteOp;
}

DeleteCommandReply DeleteOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));
    return DeleteCommandReply::parse(IDLParserContext("deleteReply"), obj);
}

void DeleteOp::validate(const DeleteCommandRequest& deleteOp) {
    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
}

}  // namespace mongo
