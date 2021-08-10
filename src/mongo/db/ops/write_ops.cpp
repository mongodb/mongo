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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops.h"

#include "mongo/db/dbmessage.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/visit_helper.h"

namespace mongo {

using write_ops::DeleteCommandRequest;
using write_ops::DeleteOpEntry;
using write_ops::InsertCommandRequest;
using write_ops::UpdateCommandRequest;
using write_ops::UpdateOpEntry;

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
                          << ". Write command: " << redact(op.toBSON({})),
            stmtIds->size() == numOps);
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "May not specify both stmtId and stmtIds in write command. Got "
                              << BSON("stmtId" << *op.getWriteCommandRequestBase().getStmtId()
                                               << "stmtIds" << *stmtIds)
                              << ". Write command: " << redact(op.toBSON({})),
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

}  // namespace write_ops

write_ops::InsertCommandRequest InsertOp::parse(const OpMsgRequest& request) {
    auto insertOp = InsertCommandRequest::parse(IDLParserErrorContext("insert"), request);

    validate(insertOp);
    return insertOp;
}

write_ops::InsertCommandRequest InsertOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    InsertCommandRequest op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandRequestBase writeCommandBase;
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

void InsertOp::validate(const write_ops::InsertCommandRequest& insertOp) {
    const auto& docs = insertOp.getDocuments();
    checkOpCountForCommand(insertOp, docs.size());
}

write_ops::UpdateCommandRequest UpdateOp::parse(const OpMsgRequest& request) {
    auto updateOp = UpdateCommandRequest::parse(IDLParserErrorContext("update"), request);

    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
    return updateOp;
}

write_ops::UpdateCommandReply UpdateOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return write_ops::UpdateCommandReply::parse(IDLParserErrorContext("updateReply"), obj);
}

void UpdateOp::validate(const UpdateCommandRequest& updateOp) {
    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
}

write_ops::FindAndModifyCommandReply FindAndModifyOp::parseResponse(const BSONObj& obj) {
    uassertStatusOK(getStatusFromCommandResult(obj));

    return write_ops::FindAndModifyCommandReply::parse(IDLParserErrorContext("findAndModifyReply"),
                                                       obj);
}

write_ops::DeleteCommandRequest DeleteOp::parse(const OpMsgRequest& request) {
    auto deleteOp = DeleteCommandRequest::parse(IDLParserErrorContext("delete"), request);

    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
    return deleteOp;
}

void DeleteOp::validate(const DeleteCommandRequest& deleteOp) {
    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
}

write_ops::UpdateModification write_ops::UpdateModification::parseFromOplogEntry(
    const BSONObj& oField, const DiffOptions& options) {
    BSONElement vField = oField[kUpdateOplogEntryVersionFieldName];

    // If this field appears it should be an integer.
    uassert(4772600,
            str::stream() << "Expected $v field to be missing or an integer, but got type: "
                          << vField.type(),
            !vField.ok() ||
                (vField.type() == BSONType::NumberInt || vField.type() == BSONType::NumberLong));

    if (vField.ok() && vField.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kDeltaV2)) {
        // Make sure there's a diff field.
        BSONElement diff = oField[update_oplog_entry::kDiffObjectFieldName];
        uassert(4772601,
                str::stream() << "Expected 'diff' field to be an object, instead got type: "
                              << diff.type(),
                diff.type() == BSONType::Object);

        return UpdateModification(doc_diff::Diff{diff.embeddedObject()}, options);
    } else if (!vField.ok() ||
               vField.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kUpdateNodeV1)) {
        // Treat it as a "classic" update which can either be a full replacement or a
        // modifier-style update. Which variant it is will be determined when the update driver is
        // constructed.
        return UpdateModification(oField, ClassicTag{});
    }

    // The $v field must be present, but have some unsupported value.
    uasserted(4772604,
              str::stream() << "Unrecognized value for '$v' (Version) field: "
                            << vField.numberInt());
}

write_ops::UpdateModification::UpdateModification(doc_diff::Diff diff, DiffOptions options)
    : _update(DeltaUpdate{std::move(diff), options}) {}

write_ops::UpdateModification::UpdateModification(BSONElement update) {
    const auto type = update.type();
    if (type == BSONType::Object) {
        _update = ClassicUpdate{update.Obj()};
        return;
    }

    uassert(ErrorCodes::FailedToParse,
            "Update argument must be either an object or an array",
            type == BSONType::Array);

    _update = PipelineUpdate{parsePipelineFromBSON(update)};
}

write_ops::UpdateModification::UpdateModification(const BSONObj& update, ClassicTag) {
    // Do a sanity check that the $v field is either not provided or has value of 1.
    const auto versionElem = update["$v"];
    uassert(4772602,
            str::stream() << "Expected classic update either contain no '$v' field, or "
                          << "'$v' field with value 1, but found: " << versionElem,
            !versionElem.ok() ||
                versionElem.numberInt() ==
                    static_cast<int>(UpdateOplogEntryVersion::kUpdateNodeV1));

    _update = ClassicUpdate{update};
}

write_ops::UpdateModification::UpdateModification(std::vector<BSONObj> pipeline)
    : _update{PipelineUpdate{std::move(pipeline)}} {}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
write_ops::UpdateModification write_ops::UpdateModification::parseFromBSON(BSONElement elem) {
    return UpdateModification(elem);
}

int write_ops::UpdateModification::objsize() const {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const ClassicUpdate& classic) -> int { return classic.bson.objsize(); },
            [](const PipelineUpdate& pipeline) -> int {
                int size = 0;
                std::for_each(pipeline.begin(), pipeline.end(), [&size](const BSONObj& obj) {
                    size += obj.objsize() + kWriteCommandBSONArrayPerElementOverheadBytes;
                });

                return size + kWriteCommandBSONArrayPerElementOverheadBytes;
            },
            [](const DeltaUpdate& delta) -> int { return delta.diff.objsize(); }},
        _update);
}


write_ops::UpdateModification::Type write_ops::UpdateModification::type() const {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const ClassicUpdate& classic) { return Type::kClassic; },
            [](const PipelineUpdate& pipelineUpdate) { return Type::kPipeline; },
            [](const DeltaUpdate& delta) { return Type::kDelta; }},
        _update);
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void write_ops::UpdateModification::serializeToBSON(StringData fieldName,
                                                    BSONObjBuilder* bob) const {

    stdx::visit(
        visit_helper::Overloaded{
            [fieldName, bob](const ClassicUpdate& classic) { *bob << fieldName << classic.bson; },
            [fieldName, bob](const PipelineUpdate& pipeline) {
                BSONArrayBuilder arrayBuilder(bob->subarrayStart(fieldName));
                for (auto&& stage : pipeline) {
                    arrayBuilder << stage;
                }
                arrayBuilder.doneFast();
            },
            [fieldName, bob](const DeltaUpdate& delta) { *bob << fieldName << delta.diff; }},
        _update);
}

}  // namespace mongo
