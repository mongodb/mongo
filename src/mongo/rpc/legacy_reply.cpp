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

#include "mongo/rpc/legacy_reply.h"

#include <tuple>
#include <utility>

#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

LegacyReply::LegacyReply(const Message* message) : _message(std::move(message)) {
    invariant(message->operation() == opReply);

    QueryResult::View qr = _message->singleData().view2ptr();

    // should be checked by caller.
    invariant(qr.msgdata().getNetworkOp() == opReply);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad cursorId field,"
                          << " expected a value of 0 but got "
                          << qr.getCursorId(),
            qr.getCursorId() == 0);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad nReturned field,"
                          << " expected a value of 1 but got "
                          << qr.getNReturned(),
            qr.getNReturned() == 1);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad startingFrom field,"
                          << " expected a value of 0 but got "
                          << qr.getStartingFrom(),
            qr.getStartingFrom() == 0);

    std::tie(_commandReply, _metadata) =
        uassertStatusOK(rpc::upconvertReplyMetadata(BSONObj(qr.data())));

    _outputDocs = DocumentRange{};
    return;
}

const BSONObj& LegacyReply::getMetadata() const {
    return _metadata;
}

const BSONObj& LegacyReply::getCommandReply() const {
    return _commandReply;
}

DocumentRange LegacyReply::getOutputDocs() const {
    return _outputDocs;
}

Protocol LegacyReply::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

}  // namespace rpc
}  // namespace mongo
