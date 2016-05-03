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

#include "mongo/executor/remote_command_response.h"

#include "mongo/rpc/reply_interface.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {

// TODO(amidvidy): we currently discard output docs when we use this constructor. We should
// have RCR hold those too, but we need more machinery before that is possible.
RemoteCommandResponse::RemoteCommandResponse(const rpc::ReplyInterface& rpcReply,
                                             Milliseconds millis)
    : RemoteCommandResponse(rpcReply.getCommandReply(), rpcReply.getMetadata(), std::move(millis)) {
}

std::string RemoteCommandResponse::toString() const {
    return str::stream() << "RemoteResponse -- "
                         << " cmd:" << data.toString();
}

bool RemoteCommandResponse::operator==(const RemoteCommandResponse& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return data == rhs.data && metadata == rhs.metadata && elapsedMillis == rhs.elapsedMillis;
}

bool RemoteCommandResponse::operator!=(const RemoteCommandResponse& rhs) const {
    return !(*this == rhs);
}

}  // namespace executor

std::ostream& operator<<(std::ostream& os, const executor::RemoteCommandResponse& response) {
    return os << response.toString();
}

}  // namespace mongo
