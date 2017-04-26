/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/rpc/command_request.h"

#include <string>
#include <utility>

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_string_data.h"
#include "mongo/base/data_type_terminated.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

namespace {
// None of these include null byte
const std::size_t kMinCommandNameLength = 1;
const std::size_t kMaxCommandNameLength = 128;

}  // namespace

CommandRequest::CommandRequest(const Message* message) : _message(message) {
    char* begin = _message->singleData().data();
    std::size_t length = _message->singleData().dataLen();

    // checked in message_port.cpp
    invariant(length <= MaxMessageSizeBytes);

    const char* messageEnd = begin + length;

    ConstDataRangeCursor cur(begin, messageEnd);

    Terminated<'\0', StringData> str;
    uassertStatusOK(cur.readAndAdvance<>(&str));
    _database = std::move(str.value);

    uassertStatusOK(cur.readAndAdvance<>(&str));
    _commandName = std::move(str.value);

    uassert(28637,
            str::stream() << "Command name parsed in OP_COMMAND message must be between "
                          << kMinCommandNameLength
                          << " and "
                          << kMaxCommandNameLength
                          << " bytes. Got: "
                          << _database,
            (_commandName.size() >= kMinCommandNameLength) &&
                (_commandName.size() <= kMaxCommandNameLength));

    Validated<BSONObj> obj;
    uassertStatusOK(cur.readAndAdvance<>(&obj));
    _commandArgs = std::move(obj.val);
    uassert(39950,
            str::stream() << "Command name parsed in OP_COMMAND message '" << _commandName
                          << "' doesn't match command name from object '"
                          << _commandArgs.firstElementFieldName()
                          << '\'',
            _commandArgs.firstElementFieldName() == _commandName);

    // OP_COMMAND is only used when communicating with 3.4 nodes and they serialize their metadata
    // fields differently. We do all up-conversion here so that the rest of the code only has to
    // deal with the current format.
    uassertStatusOK(cur.readAndAdvance<>(&obj));
    BSONObjBuilder metadataBuilder;
    for (auto elem : obj.val) {
        if (elem.fieldNameStringData() == "configsvr") {
            metadataBuilder.appendAs(elem, "$configServerState");
        } else if (elem.fieldNameStringData() == "$ssm") {
            auto ssmObj = elem.Obj();
            if (auto readPrefElem = ssmObj["$readPreference"]) {
                // Promote the read preference to the top level.
                metadataBuilder.append(readPrefElem);
            } else if (ssmObj["$secondaryOk"].trueValue()) {
                // Convert secondaryOk to equivalent read preference if none was explicitly
                // provided.
                ReadPreferenceSetting(ReadPreference::SecondaryPreferred)
                    .toContainingBSON(&metadataBuilder);
            }
        } else {
            metadataBuilder.append(elem);
        }
    }
    _metadata = metadataBuilder.obj();

    uassert(40419, "OP_COMMAND request contains trailing bytes following metadata", cur.empty());
}

StringData CommandRequest::getDatabase() const {
    return _database;
}

StringData CommandRequest::getCommandName() const {
    return _commandName;
}

const BSONObj& CommandRequest::getMetadata() const {
    return _metadata;
}

const BSONObj& CommandRequest::getCommandArgs() const {
    return _commandArgs;
}

bool operator==(const CommandRequest& lhs, const CommandRequest& rhs) {
    return (lhs._database == rhs._database) && (lhs._commandName == rhs._commandName) &&
        SimpleBSONObjComparator::kInstance.evaluate(lhs._metadata == rhs._metadata) &&
        SimpleBSONObjComparator::kInstance.evaluate(lhs._commandArgs == rhs._commandArgs);
}

bool operator!=(const CommandRequest& lhs, const CommandRequest& rhs) {
    return !(lhs == rhs);
}

Protocol CommandRequest::getProtocol() const {
    return rpc::Protocol::kOpCommandV1;
}

}  // namespace rpc
}  // namespace mongo
