/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/repl/rollback_impl_listener.h"

#include <algorithm>
#include <list>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/logger/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

namespace {

const auto kUuidFieldName = "ui"_sd;

/**
 * Parses op type of oplog entry.
 *
 * TODO: Replace with new IDL'ed OplogEntry parser.
 */
StatusWith<char> parseOpType(const BSONObj& oplogEntryObj) {
    std::string opTypeStr;
    invariantOK(mongo::bsonExtractStringField(oplogEntryObj, "op", &opTypeStr));
    invariant(opTypeStr.size() == 1U);
    auto opType = opTypeStr.front();
    switch (opType) {
        case 'c':
        case 'i':
        case 'd':
        case 'u':
        case 'n':
            return opType;
        default:
            return {ErrorCodes::UnrecoverableRollbackError,
                    str::stream() << "Unrecognized op type '" << opTypeStr << "' in oplog entry: "
                                  << redact(oplogEntryObj)};
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns true if op type represents an insert, delete or update operation on a single document.
 */
bool isCrudOpType(char opType) {
    switch (opType) {
        case 'd':
        case 'i':
        case 'u':
            return true;
        default:
            return false;
    }
    MONGO_UNREACHABLE;
}

/**
 * Returns true if command oplog entry supports collection UUID.
 */
bool commandSupportsCollectionUuid(const BSONObj& oplogEntryObj) {
    invariant("c" == oplogEntryObj["op"].str());
    auto commandName = oplogEntryObj["o"].Obj().firstElementFieldName();
    std::list<std::string> commandsWithUuidSupport({"create",
                                                    "renameCollection",
                                                    "drop",
                                                    "collMod",
                                                    "emptycapped",
                                                    "convertToCapped",
                                                    "createIndex",
                                                    "dropIndexes"});
    return std::find(commandsWithUuidSupport.cbegin(),
                     commandsWithUuidSupport.cend(),
                     commandName) != commandsWithUuidSupport.cend();
}

/**
 * Performs basic validation on the provided oplog entry.
 * Checks for unrecognized op types and also for the presence of a collection UUID (if applicable).
 */
Status validateOplogEntry(const BSONObj& oplogEntryObj) {
    auto opTypeResult = parseOpType(oplogEntryObj);
    if (!opTypeResult.isOK()) {
        return opTypeResult.getStatus();
    }
    auto opType = opTypeResult.getValue();
    if (isCrudOpType(opType) || commandSupportsCollectionUuid(oplogEntryObj)) {
        auto uuidResult = UUID::parse(oplogEntryObj[kUuidFieldName]);
        if (!uuidResult.isOK()) {
            return {ErrorCodes::IncompatibleRollbackAlgorithm,
                    str::stream() << "Oplog entry contains malformed or missing collection UUID: "
                                  << redact(oplogEntryObj)};
        }
    } else if ("applyOps" == oplogEntryObj["o"].Obj().firstElement().fieldNameStringData()) {
        invariant('c' == opType);
        auto operations = oplogEntryObj["o"].Obj().firstElement().Obj();
        for (auto element : operations) {
            invariant(element.isABSONObj());
            auto status = validateOplogEntry(element.Obj());
            if (!status.isOK()) {
                return status;
            }
        }
    }
    return Status::OK();
}

}  // namespace

Status RollbackImpl::Listener::onLocalOplogEntry(const BSONObj& oplogEntryObj) {
    return validateOplogEntry(oplogEntryObj);
}

Status RollbackImpl::Listener::onRemoteOplogEntry(const BSONObj& oplogEntryObj) {
    return validateOplogEntry(oplogEntryObj);
}

Status RollbackImpl::Listener::onCommonPoint(
    const RollbackCommonPointResolver::RollbackCommonPoint& oplogEntryObj) {
    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
