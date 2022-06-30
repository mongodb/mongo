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

#include "mongo/db/dbmessage.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/visit_helper.h"

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
    return stdx::visit(
        visit_helper::Overloaded{
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
            [](const TransformUpdate& transform) -> int { return 0; }},
        _update);
}

UpdateModification::Type UpdateModification::type() const {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const ReplacementUpdate& replacement) { return Type::kReplacement; },
            [](const ModifierUpdate& modifier) { return Type::kModifier; },
            [](const PipelineUpdate& pipelineUpdate) { return Type::kPipeline; },
            [](const DeltaUpdate& delta) { return Type::kDelta; },
            [](const TransformUpdate& transform) { return Type::kTransform; }},
        _update);
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void UpdateModification::serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const {

    stdx::visit(
        visit_helper::Overloaded{
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
            [](const TransformUpdate& transform) {}},
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
    auto insertOp = InsertCommandRequest::parse(IDLParserErrorContext("insert"), request);

    validate(insertOp);
    return insertOp;
}

InsertCommandRequest InsertOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    InsertCommandRequest op(NamespaceString(msg.getns()));

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
    return InsertCommandReply::parse(IDLParserErrorContext("insertReply"), obj);
}

void InsertOp::validate(const InsertCommandRequest& insertOp) {
    const auto& docs = insertOp.getDocuments();
    checkOpCountForCommand(insertOp, docs.size());
}

UpdateCommandRequest UpdateOp::parse(const OpMsgRequest& request) {
    auto updateOp = UpdateCommandRequest::parse(IDLParserErrorContext("update"), request);

    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
    return updateOp;
}

UpdateCommandReply UpdateOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return UpdateCommandReply::parse(IDLParserErrorContext("updateReply"), obj);
}

void UpdateOp::validate(const UpdateCommandRequest& updateOp) {
    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
}

FindAndModifyCommandReply FindAndModifyOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return FindAndModifyCommandReply::parse(IDLParserErrorContext("findAndModifyReply"), obj);
}

DeleteCommandRequest DeleteOp::parse(const OpMsgRequest& request) {
    auto deleteOp = DeleteCommandRequest::parse(IDLParserErrorContext("delete"), request);

    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
    return deleteOp;
}

DeleteCommandReply DeleteOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));
    return DeleteCommandReply::parse(IDLParserErrorContext("deleteReply"), obj);
}

void DeleteOp::validate(const DeleteCommandRequest& deleteOp) {
    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
}

}  // namespace mongo
