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

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/document_validation.h"
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

void checkType(BSONType expectedType, const BSONElement& elem) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Wrong type for '" << elem.fieldNameStringData() << "'. Expected a "
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
void parseWriteCommand(StringData dbName,
                       const BSONObj& cmd,
                       StringData uniqueFieldName,
                       BSONElement* uniqueField,
                       ParsedWriteOp* op) {
    // Command dispatch wouldn't get here with an empty object because the first field indicates
    // which command to run.
    invariant(!cmd.isEmpty());

    bool haveUniqueField = false;
    bool firstElement = true;
    for (BSONElement field : cmd) {
        if (firstElement) {
            // The key is the command name and the value is the collection name
            checkType(String, field);
            op->ns = NamespaceString(dbName, field.valueStringData());
            firstElement = false;
            continue;
        }

        const StringData fieldName = field.fieldNameStringData();
        if (fieldName == "bypassDocumentValidation") {
            op->bypassDocumentValidation = field.trueValue();
        } else if (fieldName == "ordered") {
            checkType(Bool, field);
            op->continueOnError = !field.Bool();
        } else if (fieldName == uniqueFieldName) {
            haveUniqueField = true;
            *uniqueField = field;
        } else if (fieldName[0] != '$') {
            std::initializer_list<StringData> ignoredFields = {
                "writeConcern", "maxTimeMS", "shardVersion"};
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Unknown option to " << cmd.firstElementFieldName()
                                  << " command: "
                                  << fieldName,
                    std::find(ignoredFields.begin(), ignoredFields.end(), fieldName) !=
                        ignoredFields.end());
        }
    }

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << uniqueFieldName << " option is required to the "
                          << cmd.firstElementFieldName()
                          << " command.",
            haveUniqueField);
}
}

InsertOp parseInsertCommand(StringData dbName, const BSONObj& cmd) {
    BSONElement documents;
    InsertOp op;
    parseWriteCommand(dbName, cmd, "documents", &documents, &op);
    checkType(Array, documents);
    for (auto doc : documents.Obj()) {
        checkTypeInArray(Object, doc, documents);
        op.documents.push_back(doc.Obj());
    }
    checkOpCountForCommand(op.documents.size());

    if (op.ns.isSystemDotIndexes()) {
        // This is only for consistency with sharding.
        uassert(ErrorCodes::InvalidLength,
                "Insert commands to system.indexes are limited to a single insert",
                op.documents.size() == 1);
    }

    return op;
}

UpdateOp parseUpdateCommand(StringData dbName, const BSONObj& cmd) {
    BSONElement updates;
    UpdateOp op;
    parseWriteCommand(dbName, cmd, "updates", &updates, &op);
    checkType(Array, updates);
    for (auto doc : updates.Obj()) {
        checkTypeInArray(Object, doc, updates);
        op.updates.emplace_back();
        auto& update = op.updates.back();
        bool haveQ = false;
        bool haveU = false;
        for (auto field : doc.Obj()) {
            const StringData fieldName = field.fieldNameStringData();
            if (fieldName == "q") {
                haveQ = true;
                checkType(Object, field);
                update.query = field.Obj();
            } else if (fieldName == "u") {
                haveU = true;
                checkType(Object, field);
                update.update = field.Obj();
            } else if (fieldName == "collation") {
                checkType(Object, field);
                update.collation = field.Obj();
            } else if (fieldName == "multi") {
                checkType(Bool, field);
                update.multi = field.Bool();
            } else if (fieldName == "upsert") {
                checkType(Bool, field);
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

DeleteOp parseDeleteCommand(StringData dbName, const BSONObj& cmd) {
    BSONElement deletes;
    DeleteOp op;
    parseWriteCommand(dbName, cmd, "deletes", &deletes, &op);
    checkType(Array, deletes);
    for (auto doc : deletes.Obj()) {
        checkTypeInArray(Object, doc, deletes);
        op.deletes.emplace_back();
        auto& del = op.deletes.back();  // delete is a reserved word.
        bool haveQ = false;
        bool haveLimit = false;
        for (auto field : doc.Obj()) {
            const StringData fieldName = field.fieldNameStringData();
            if (fieldName == "q") {
                haveQ = true;
                checkType(Object, field);
                del.query = field.Obj();
            } else if (fieldName == "collation") {
                checkType(Object, field);
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
