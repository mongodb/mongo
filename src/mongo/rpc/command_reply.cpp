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

#include "mongo/rpc/command_reply.h"

#include <tuple>
#include <utility>

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

CommandReply::CommandReply(const Message* message) : _message(message) {
    const char* begin = _message->singleData().data();
    std::size_t length = _message->singleData().dataLen();

    // This check failing would normally be operation fatal, but we expect it to have been
    // done earlier in the network layer, so we make it an invariant.
    invariant(length <= MaxMessageSizeBytes);

    const char* messageEnd = begin + length;
    ConstDataRangeCursor cur(begin, messageEnd);

    _commandReply = uassertStatusOK(cur.readAndAdvance<Validated<BSONObj>>()).val;
    _commandReply.shareOwnershipWith(message->sharedBuffer());

    // OP_COMMAND is only used when communicating with 3.4 nodes and they serialize their metadata
    // fields differently. We do all up- and down-conversion here so that the rest of the code only
    // has to deal with the current format.
    auto rawMetadata = uassertStatusOK(cur.readAndAdvance<Validated<BSONObj>>()).val;
    BSONObjBuilder metadataBuilder;
    for (auto elem : rawMetadata) {
        if (elem.fieldNameStringData() == "configsvr") {
            metadataBuilder.appendAs(elem, "$configServerState");
        } else {
            metadataBuilder.append(elem);
        }
    }
    _metadata = metadataBuilder.obj();

    uassert(40420, "OP_COMMAND reply contains trailing bytes following metadata", cur.empty());
}

const BSONObj& CommandReply::getMetadata() const {
    return _metadata;
}

const BSONObj& CommandReply::getCommandReply() const {
    return _commandReply;
}

Protocol CommandReply::getProtocol() const {
    return rpc::Protocol::kOpCommandV1;
}

bool operator==(const CommandReply& lhs, const CommandReply& rhs) {
    SimpleBSONObjComparator bsonComparator;
    return bsonComparator.evaluate(lhs._metadata == rhs._metadata) &&
        bsonComparator.evaluate(lhs._commandReply == rhs._commandReply);
}

bool operator!=(const CommandReply& lhs, const CommandReply& rhs) {
    return !(lhs == rhs);
}

}  // namespace rpc
}  // namespace mongo
