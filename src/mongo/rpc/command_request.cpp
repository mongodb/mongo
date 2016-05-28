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
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

namespace {
// None of these include null byte
const std::size_t kMaxDatabaseLength = 63;
const std::size_t kMinDatabaseLength = 1;

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

    uassert(28636,
            str::stream() << "Database parsed in OP_COMMAND message must be between"
                          << kMinDatabaseLength
                          << " and "
                          << kMaxDatabaseLength
                          << " bytes. Got: "
                          << _database,
            (_database.size() >= kMinDatabaseLength) && (_database.size() <= kMaxDatabaseLength));

    uassert(
        ErrorCodes::InvalidNamespace,
        str::stream() << "Invalid database name: '" << _database << "'",
        NamespaceString::validDBName(_database, NamespaceString::DollarInDbNameBehavior::Allow));

    uassertStatusOK(cur.readAndAdvance<>(&str));
    _commandName = std::move(str.value);

    uassert(28637,
            str::stream() << "Command name parsed in OP_COMMAND message must be between"
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

    uassertStatusOK(cur.readAndAdvance<>(&obj));
    _metadata = std::move(obj.val);

    _inputDocs = DocumentRange{cur.data(), messageEnd};
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

DocumentRange CommandRequest::getInputDocs() const {
    return _inputDocs;
}

bool operator==(const CommandRequest& lhs, const CommandRequest& rhs) {
    return std::tie(
               lhs._database, lhs._commandName, lhs._metadata, lhs._commandArgs, lhs._inputDocs) ==
        std::tie(rhs._database, rhs._commandName, rhs._metadata, rhs._commandArgs, rhs._inputDocs);
}

bool operator!=(const CommandRequest& lhs, const CommandRequest& rhs) {
    return !(lhs == rhs);
}

Protocol CommandRequest::getProtocol() const {
    return rpc::Protocol::kOpCommandV1;
}

}  // namespace rpc
}  // namespace mongo
