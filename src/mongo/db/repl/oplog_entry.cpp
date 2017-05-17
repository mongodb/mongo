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
    } else if (commandString == "dropDatabase") {
        return OplogEntry::CommandType::kDropDatabase;
    } else if (commandString == "emptycapped") {
        return OplogEntry::CommandType::kEmptyCapped;
    } else if (commandString == "convertToCapped") {
        return OplogEntry::CommandType::kConvertToCapped;
    } else if (commandString == "createIndex") {
        return OplogEntry::CommandType::kCreateIndex;
    } else if (commandString == "dropIndexes") {
        return OplogEntry::CommandType::kDropIndexes;
    } else if (commandString == "deleteIndexes") {
        return OplogEntry::CommandType::kDropIndexes;
    } else {
        severe() << "Unknown oplog entry command type: " << commandString
                 << " Object field: " << redact(objectField);
        fassertFailedNoTrace(40444);
    }
    MONGO_UNREACHABLE;
}

}  // namespace

const int OplogEntry::kOplogVersion = 2;

// Static
StatusWith<OplogEntry> OplogEntry::parse(const BSONObj& object) {
    try {
        return OplogEntry(object);
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
}

OplogEntry::OplogEntry(BSONObj rawInput)
    : raw(std::move(rawInput)), _commandType(OplogEntry::CommandType::kNotCommand) {
    raw = raw.getOwned();

    parseProtected(IDLParserErrorContext("OplogEntryBase"), raw);

    // Parse command type from 'o' and 'o2' fields.
    if (isCommand()) {
        _commandType = parseCommandType(getObject());
    }
}
OplogEntry::OplogEntry(OpTime opTime,
                       long long hash,
                       OpTypeEnum opType,
                       NamespaceString nss,
                       int version,
                       const BSONObj& oField,
                       const BSONObj& o2Field) {
    setTimestamp(opTime.getTimestamp());
    setTerm(opTime.getTerm());
    setHash(hash);
    setOpType(opType);
    setNamespace(nss);
    setVersion(version);
    setObject(oField);
    setObject2(o2Field);

    // This is necessary until we remove `raw` in SERVER-29200.
    raw = toBSON();
}

OplogEntry::OplogEntry(OpTime opTime,
                       long long hash,
                       OpTypeEnum opType,
                       NamespaceString nss,
                       int version,
                       const BSONObj& oField)
    : OplogEntry(opTime, hash, opType, nss, version, oField, BSONObj()) {}

OplogEntry::OplogEntry(
    OpTime opTime, long long hash, OpTypeEnum opType, NamespaceString nss, const BSONObj& oField)
    : OplogEntry(opTime, hash, opType, nss, OplogEntry::kOplogVersion, oField, BSONObj()) {}

OplogEntry::OplogEntry(OpTime opTime,
                       long long hash,
                       OpTypeEnum opType,
                       NamespaceString nss,
                       const BSONObj& oField,
                       const BSONObj& o2Field)
    : OplogEntry(opTime, hash, opType, nss, OplogEntry::kOplogVersion, oField, o2Field) {}

bool OplogEntry::isCommand() const {
    return getOpType() == OpTypeEnum::kCommand;
}

bool OplogEntry::isCrudOpType() const {
    switch (getOpType()) {
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

BSONElement OplogEntry::getIdElement() const {
    invariant(isCrudOpType());
    if (getOpType() == OpTypeEnum::kUpdate) {
        return getObject2()->getField("_id");
    } else {
        return getObject()["_id"];
    }
}

OplogEntry::CommandType OplogEntry::getCommandType() const {
    invariant(isCommand());
    invariant(_commandType != OplogEntry::CommandType::kNotCommand);
    return _commandType;
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

}  // namespace repl
}  // namespace mongo
