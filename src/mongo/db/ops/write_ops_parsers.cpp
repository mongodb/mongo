/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops_parsers.h"

#include "mongo/bson/util/bson_check.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbmessage.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

// The specified limit to the number of operations that can be included in a single write command.
// This is an attempt to avoid a large number of errors resulting in a reply that exceeds 16MB. It
// doesn't fully ensure that goal, but it reduces the probability of it happening. This limit should
// not be used if the protocol changes to avoid the 16MB limit on reply size.
const size_t kMaxWriteBatchSize = 1000;

void checkTypeInArray(BSONType expectedType,
                      const BSONElement& elem,
                      const BSONElement& arrayElem) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Wrong type for " << arrayElem.fieldNameStringData() << '['
                          << elem.fieldNameStringData()
                          << "]. Expected a "
                          << typeName(expectedType)
                          << ", got a "
                          << typeName(elem.type())
                          << '.',
            elem.type() == expectedType);
}

void checkOpCountForCommand(size_t numOps) {
    uassert(ErrorCodes::InvalidLength,
            str::stream() << "Write batch sizes must be between 1 and " << kMaxWriteBatchSize
                          << ". Got "
                          << numOps
                          << " operations.",
            numOps != 0 && numOps <= kMaxWriteBatchSize);
}

/**
 * Parses the fields common to all write commands and sets uniqueField to the element named
 * uniqueFieldName. The uniqueField is the only top-level field that is unique to the specific type
 * of write command.
 */
void parseWriteCommand(const OpMsgRequest& request,
                       StringData uniqueFieldName,
                       std::vector<BSONObj>* uniqueField,
                       ParsedWriteOp* op) {
    // Command dispatch wouldn't get here with an empty object because the first field indicates
    // which command to run.
    invariant(!request.body.isEmpty());

    bool haveUniqueField = false;
    bool firstElement = true;
    for (BSONElement field : request.body) {
        if (firstElement) {
            // The key is the command name and the value is the collection name
            checkBSONType(String, field);
            op->ns = NamespaceString(request.getDatabase(), field.valueStringData());
            uassert(ErrorCodes::InvalidNamespace,
                    str::stream() << "Invalid namespace: " << op->ns.ns(),
                    op->ns.isValid());
            firstElement = false;
            continue;
        }

        const StringData fieldName = field.fieldNameStringData();
        if (fieldName == "bypassDocumentValidation") {
            op->bypassDocumentValidation = field.trueValue();
        } else if (fieldName == "ordered") {
            checkBSONType(Bool, field);
            op->continueOnError = !field.Bool();
        } else if (fieldName == uniqueFieldName) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Duplicate field " << uniqueFieldName,
                    !haveUniqueField);
            haveUniqueField = true;
            checkBSONType(Array, field);
            for (auto subField : field.Obj()) {
                checkTypeInArray(Object, subField, field);
                uniqueField->push_back(subField.Obj());
            }
        } else if (!Command::isGenericArgument(fieldName)) {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "Unknown option to " << request.getCommandName()
                                    << " command: "
                                    << fieldName);
        }
    }

    for (auto&& seq : request.sequences) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Unknown document sequence option to " << request.getCommandName()
                              << " command: "
                              << seq.name,
                seq.name == uniqueFieldName);
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "Duplicate document sequence " << uniqueFieldName,
                !haveUniqueField);
        haveUniqueField = true;
        *uniqueField = seq.objs;
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << uniqueFieldName << " option is required to the "
                          << request.getCommandName()
                          << " command.",
            haveUniqueField);
}
}

InsertOp parseInsertCommand(const OpMsgRequest& request) {
    invariant(request.getCommandName() == "insert");
    InsertOp op;
    parseWriteCommand(request, "documents", &op.documents, &op);
    checkOpCountForCommand(op.documents.size());

    if (op.ns.isSystemDotIndexes()) {
        // This is only for consistency with sharding.
        uassert(ErrorCodes::InvalidLength,
                "Insert commands to system.indexes are limited to a single insert",
                op.documents.size() == 1);
    }

    return op;
}

UpdateOp parseUpdateCommand(const OpMsgRequest& request) {
    invariant(request.getCommandName() == "update");
    std::vector<BSONObj> updates;
    UpdateOp op;
    parseWriteCommand(request, "updates", &updates, &op);
    for (auto&& doc : updates) {
        op.updates.emplace_back();
        auto& update = op.updates.back();
        bool haveQ = false;
        bool haveU = false;
        for (auto field : doc) {
            const StringData fieldName = field.fieldNameStringData();
            if (fieldName == "q") {
                haveQ = true;
                checkBSONType(Object, field);
                update.query = field.Obj();
            } else if (fieldName == "u") {
                haveU = true;
                checkBSONType(Object, field);
                update.update = field.Obj();
            } else if (fieldName == "collation") {
                checkBSONType(Object, field);
                update.collation = field.Obj();
            } else if (fieldName == "arrayFilters") {
                checkBSONType(Array, field);
                for (auto arrayFilter : field.Obj()) {
                    checkBSONType(Object, arrayFilter);
                    update.arrayFilters.push_back(arrayFilter.Obj());
                }
            } else if (fieldName == "multi") {
                checkBSONType(Bool, field);
                update.multi = field.Bool();
            } else if (fieldName == "upsert") {
                checkBSONType(Bool, field);
                update.upsert = field.Bool();
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "Unrecognized field in update operation: " << fieldName);
            }
        }

        uassert(ErrorCodes::FailedToParse, "The 'q' field is required for all updates", haveQ);
        uassert(ErrorCodes::FailedToParse, "The 'u' field is required for all updates", haveU);
    }
    checkOpCountForCommand(op.updates.size());
    return op;
}

DeleteOp parseDeleteCommand(const OpMsgRequest& request) {
    invariant(request.getCommandName() == "delete");
    std::vector<BSONObj> deletes;
    DeleteOp op;
    parseWriteCommand(request, "deletes", &deletes, &op);
    for (auto&& doc : deletes) {
        op.deletes.emplace_back();
        auto& del = op.deletes.back();  // delete is a reserved word.
        bool haveQ = false;
        bool haveLimit = false;
        for (auto field : doc) {
            const StringData fieldName = field.fieldNameStringData();
            if (fieldName == "q") {
                haveQ = true;
                checkBSONType(Object, field);
                del.query = field.Obj();
            } else if (fieldName == "collation") {
                checkBSONType(Object, field);
                del.collation = field.Obj();
            } else if (fieldName == "limit") {
                haveLimit = true;
                uassert(ErrorCodes::TypeMismatch,
                        str::stream()
                            << "The limit field in delete objects must be a number. Got a "
                            << typeName(field.type()),
                        field.isNumber());

                // Using a double to avoid throwing away illegal fractional portion. Don't want to
                // accept 0.5 here.
                const double limit = field.numberDouble();
                uassert(ErrorCodes::FailedToParse,
                        str::stream() << "The limit field in delete objects must be 0 or 1. Got "
                                      << limit,
                        limit == 0 || limit == 1);
                del.multi = (limit == 0);
            } else {
                uasserted(ErrorCodes::FailedToParse,
                          str::stream() << "Unrecognized field in delete operation: " << fieldName);
            }
        }
        uassert(ErrorCodes::FailedToParse, "The 'q' field is required for all deletes", haveQ);
        uassert(
            ErrorCodes::FailedToParse, "The 'limit' field is required for all deletes", haveLimit);
    }
    checkOpCountForCommand(op.deletes.size());
    return op;
}

InsertOp parseLegacyInsert(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    InsertOp op;
    op.ns = NamespaceString(msg.getns());
    op.continueOnError = msg.reservedField() & InsertOption_ContinueOnError;
    uassert(ErrorCodes::InvalidLength, "Need at least one object to insert", msg.moreJSObjs());
    while (msg.moreJSObjs()) {
        op.documents.push_back(msg.nextJsObj());
    }
    // There is no limit on the number of inserts in a legacy batch.

    return op;
}

UpdateOp parseLegacyUpdate(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    UpdateOp op;
    op.ns = NamespaceString(msg.getns());

    // Legacy updates only allowed one update per operation. Layout is flags, query, update.
    op.updates.emplace_back();
    auto& singleUpdate = op.updates.back();
    const int flags = msg.pullInt();
    singleUpdate.upsert = flags & UpdateOption_Upsert;
    singleUpdate.multi = flags & UpdateOption_Multi;
    singleUpdate.query = msg.nextJsObj();
    singleUpdate.update = msg.nextJsObj();

    return op;
}

DeleteOp parseLegacyDelete(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    DeleteOp op;
    op.ns = NamespaceString(msg.getns());

    // Legacy deletes only allowed one delete per operation. Layout is flags, query.
    op.deletes.emplace_back();
    auto& singleDelete = op.deletes.back();
    const int flags = msg.pullInt();
    singleDelete.multi = !(flags & RemoveOption_JustOne);
    singleDelete.query = msg.nextJsObj();

    return op;
}

}  // namespace mongo
