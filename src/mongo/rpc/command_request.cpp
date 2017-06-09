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

ParsedOpCommand ParsedOpCommand::parse(const Message& message) {
    ParsedOpCommand out;

    char* begin = message.singleData().data();
    std::size_t length = message.singleData().dataLen();

    // checked in message_port.cpp
    invariant(length <= MaxMessageSizeBytes);

    const char* messageEnd = begin + length;

    ConstDataRangeCursor cur(begin, messageEnd);

    Terminated<'\0', StringData> str;
    uassertStatusOK(cur.readAndAdvance<>(&str));
    out.database = str.value.toString();

    uassertStatusOK(cur.readAndAdvance<>(&str));
    const auto commandName = std::move(str.value);

    uassert(28637,
            str::stream() << "Command name parsed in OP_COMMAND message must be between "
                          << kMinCommandNameLength
                          << " and "
                          << kMaxCommandNameLength
                          << " bytes. Got: "
                          << out.database,
            (commandName.size() >= kMinCommandNameLength) &&
                (commandName.size() <= kMaxCommandNameLength));

    Validated<BSONObj> obj;
    uassertStatusOK(cur.readAndAdvance<>(&obj));
    out.body = std::move(obj.val);
    uassert(39950,
            str::stream() << "Command name parsed in OP_COMMAND message '" << commandName
                          << "' doesn't match command name from object '"
                          << out.body.firstElementFieldName()
                          << '\'',
            out.body.firstElementFieldName() == commandName);

    uassertStatusOK(cur.readAndAdvance<>(&obj));
    out.metadata = std::move(obj.val);

    uassert(40419, "OP_COMMAND request contains trailing bytes following metadata", cur.empty());

    return out;
}

OpMsgRequest opMsgRequestFromCommandRequest(const Message& message) {
    auto parsed = ParsedOpCommand::parse(message);

    BSONObjBuilder bodyBuilder(std::move(parsed.body));

    // OP_COMMAND is only used when communicating with 3.4 nodes and they serialize their metadata
    // fields differently. We do all up-conversion here so that the rest of the code only has to
    // deal with the current format.
    for (auto elem : parsed.metadata) {
        if (elem.fieldNameStringData() == "configsvr") {
            bodyBuilder.appendAs(elem, "$configServerState");
        } else if (elem.fieldNameStringData() == "$ssm") {
            auto ssmObj = elem.Obj();
            if (auto readPrefElem = ssmObj["$readPreference"]) {
                // Promote the read preference to the top level.
                bodyBuilder.append(readPrefElem);
            } else if (ssmObj["$secondaryOk"].trueValue()) {
                // Convert secondaryOk to equivalent read preference if none was explicitly
                // provided.
                ReadPreferenceSetting(ReadPreference::SecondaryPreferred)
                    .toContainingBSON(&bodyBuilder);
            }
        } else {
            bodyBuilder.append(elem);
        }
    }

    bodyBuilder.append("$db", parsed.database);

    OpMsgRequest request;
    request.body = bodyBuilder.obj();
    return request;
}

}  // namespace rpc
}  // namespace mongo
