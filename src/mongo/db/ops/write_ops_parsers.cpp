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

#include "mongo/db/ops/write_ops_parsers.h"

#include "mongo/db/dbmessage.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/update_oplog_entry_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/visit_helper.h"

namespace mongo {

using write_ops::Delete;
using write_ops::DeleteOpEntry;
using write_ops::Insert;
using write_ops::Update;
using write_ops::UpdateOpEntry;

namespace {

template <class T>
void checkOpCountForCommand(const T& op, size_t numOps) {
    uassert(ErrorCodes::InvalidLength,
            str::stream() << "Write batch sizes must be between 1 and "
                          << write_ops::kMaxWriteBatchSize << ". Got " << numOps << " operations.",
            numOps != 0 && numOps <= write_ops::kMaxWriteBatchSize);

    const auto& stmtIds = op.getWriteCommandBase().getStmtIds();
    uassert(ErrorCodes::InvalidLength,
            "Number of statement ids must match the number of batch entries",
            !stmtIds || stmtIds->size() == numOps);
    uassert(ErrorCodes::InvalidOptions,
            "May not specify both stmtId and stmtIds in write command",
            !stmtIds || !op.getWriteCommandBase().getStmtId());
}

void validateInsertOp(const write_ops::Insert& insertOp) {
    const auto& docs = insertOp.getDocuments();
    checkOpCountForCommand(insertOp, docs.size());
}

}  // namespace

namespace write_ops {

bool readMultiDeleteProperty(const BSONElement& limitElement) {
    // Using a double to avoid throwing away illegal fractional portion. Don't want to accept 0.5
    // here
    const double limit = limitElement.numberDouble();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The limit field in delete objects must be 0 or 1. Got " << limit,
            limit == 0 || limit == 1);

    return limit == 0;
}

void writeMultiDeleteProperty(bool isMulti, StringData fieldName, BSONObjBuilder* builder) {
    builder->append(fieldName, isMulti ? 0 : 1);
}

int32_t getStmtIdForWriteAt(const WriteCommandBase& writeCommandBase, size_t writePos) {
    const auto& stmtIds = writeCommandBase.getStmtIds();

    if (stmtIds) {
        return stmtIds->at(writePos);
    }

    const auto& stmtId = writeCommandBase.getStmtId();
    const int32_t kFirstStmtId = stmtId ? *stmtId : 0;
    return kFirstStmtId + writePos;
}

}  // namespace write_ops

write_ops::Insert InsertOp::parse(const OpMsgRequest& request) {
    auto insertOp = Insert::parse(IDLParserErrorContext("insert"), request);

    validateInsertOp(insertOp);
    return insertOp;
}

write_ops::Insert InsertOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    Insert op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(!(msg.reservedField() & InsertOption_ContinueOnError));
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    uassert(ErrorCodes::InvalidLength, "Need at least one object to insert", msg.moreJSObjs());

    op.setDocuments([&] {
        std::vector<BSONObj> documents;
        while (msg.moreJSObjs()) {
            documents.push_back(msg.nextJsObj());
        }

        return documents;
    }());

    validateInsertOp(op);
    return op;
}

write_ops::Update UpdateOp::parse(const OpMsgRequest& request) {
    auto updateOp = Update::parse(IDLParserErrorContext("update"), request);

    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
    return updateOp;
}

write_ops::Update UpdateOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    Update op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(true);
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    op.setUpdates([&] {
        std::vector<write_ops::UpdateOpEntry> updates;
        updates.emplace_back();

        // Legacy updates only allowed one update per operation. Layout is flags, query, update.
        auto& singleUpdate = updates.back();
        const int flags = msg.pullInt();
        singleUpdate.setUpsert(flags & UpdateOption_Upsert);
        singleUpdate.setMulti(flags & UpdateOption_Multi);
        singleUpdate.setQ(msg.nextJsObj());
        singleUpdate.setU(
            write_ops::UpdateModification::parseLegacyOpUpdateFromBSON(msg.nextJsObj()));

        return updates;
    }());

    return op;
}

write_ops::Delete DeleteOp::parse(const OpMsgRequest& request) {
    auto deleteOp = Delete::parse(IDLParserErrorContext("delete"), request);

    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
    return deleteOp;
}

write_ops::Delete DeleteOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    Delete op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(true);
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    op.setDeletes([&] {
        std::vector<write_ops::DeleteOpEntry> deletes;
        deletes.emplace_back();

        // Legacy deletes only allowed one delete per operation. Layout is flags, query.
        auto& singleDelete = deletes.back();
        const int flags = msg.pullInt();
        singleDelete.setMulti(!(flags & RemoveOption_JustOne));
        singleDelete.setQ(msg.nextJsObj());

        return deletes;
    }());

    return op;
}

write_ops::UpdateModification write_ops::UpdateModification::parseFromOplogEntry(
    const BSONObj& oField) {
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

        return UpdateModification(doc_diff::Diff{diff.embeddedObject()}, DiffTag{});
    } else if (!vField.ok() ||
               vField.numberInt() == static_cast<int>(UpdateOplogEntryVersion::kUpdateNodeV1)) {
        // Treat it as a "classic" update which can either be a full replacement or a
        // modifier-style update. Which variant it is will be determined when the update driver is
        // constructed.
        return UpdateModification(oField);
    }

    // The $v field must be present, but have some unsupported value.
    uasserted(4772604,
              str::stream() << "Unrecognized value for '$v' (Version) field: "
                            << vField.numberInt());
}

write_ops::UpdateModification::UpdateModification(doc_diff::Diff diff, DiffTag)
    : _update(std::move(diff)) {}

write_ops::UpdateModification::UpdateModification(BSONElement update) {
    const auto type = update.type();
    if (type == BSONType::Object) {
        _update = ClassicUpdate{update.Obj()};
        return;
    }

    uassert(ErrorCodes::FailedToParse,
            "Update argument must be either an object or an array",
            type == BSONType::Array);

    _update = PipelineUpdate{uassertStatusOK(AggregationRequest::parsePipelineFromBSON(update))};
}

write_ops::UpdateModification::UpdateModification(const BSONObj& update) {
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

write_ops::UpdateModification write_ops::UpdateModification::parseFromBSON(BSONElement elem) {
    return UpdateModification(elem);
}

write_ops::UpdateModification write_ops::UpdateModification::parseLegacyOpUpdateFromBSON(
    const BSONObj& obj) {
    return UpdateModification(obj);
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
            [](const doc_diff::Diff& diff) -> int { return diff.objsize(); }},
        _update);
}


write_ops::UpdateModification::Type write_ops::UpdateModification::type() const {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const ClassicUpdate& classic) { return Type::kClassic; },
            [](const PipelineUpdate& pipelineUpdate) { return Type::kPipeline; },
            [](const doc_diff::Diff& diff) { return Type::kDelta; }},
        _update);
}

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
            [](const doc_diff::Diff& diff) {
                // We never serialize delta style updates.
                MONGO_UNREACHABLE;
            }},
        _update);
}

}  // namespace mongo
