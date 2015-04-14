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

#include "mongo/rpc/reply.h"

#include <tuple>

#include "mongo/base/data_range_cursor.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

    Reply::Reply(const Message* message)
        : _message(message) {

        char* begin = _message->singleData().data();
        std::size_t length = _message->singleData().dataLen();

        invariant(length <= MaxMessageSizeBytes); // checked by message_port.cpp

        char* messageEnd = begin + length;
        ConstDataRangeCursor cur(begin, messageEnd);

        // TODO(amidvidy): we don't currently handle BSON validation.
        // we will eventually have to thread serverGlobalParams.objcheck through here
        // similarly to DbMessage::nextJsObj
        uassertStatusOK(cur.readAndAdvance<BSONObj>(&_metadata));
        uassertStatusOK(cur.readAndAdvance<BSONObj>(&_commandReply));
        _outputDocs = DocumentRange(cur.data(), messageEnd);
    }

    const BSONObj& Reply::getMetadata() const {
        return _metadata;
    }

    const BSONObj& Reply::getCommandReply() const {
        return _commandReply;
    }

    DocumentRange Reply::getOutputDocs() const {
        return  _outputDocs;
    }

    bool operator==(const Reply& lhs, const Reply& rhs) {
        return std::tie(lhs._metadata, lhs._commandReply, lhs._outputDocs) ==
               std::tie(rhs._metadata, rhs._commandReply, rhs._outputDocs);
    }

    bool operator!=(const Reply& lhs, const Reply& rhs) {
        return !(lhs == rhs);
    }

}  // namespace rpc
}  // namespace mongo
