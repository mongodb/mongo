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

#include "mongo/executor/remote_command_request.h"

#include <ostream>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {
namespace {

// Used to generate unique identifiers for requests so they can be traced throughout the
// asynchronous networking logs
AtomicUInt64 requestIdCounter(0);

}  // namespace

const Milliseconds RemoteCommandRequest::kNoTimeout{-1};
const Date_t RemoteCommandRequest::kNoExpirationDate{Date_t::max()};

RemoteCommandRequest::RemoteCommandRequest() : id(requestIdCounter.addAndFetch(1)) {}

RemoteCommandRequest::RemoteCommandRequest(RequestId requestId,
                                           const HostAndPort& theTarget,
                                           const std::string& theDbName,
                                           const BSONObj& theCmdObj,
                                           const BSONObj& metadataObj,
                                           Milliseconds timeoutMillis)
    : id(requestId),
      target(theTarget),
      dbname(theDbName),
      metadata(metadataObj),
      cmdObj(theCmdObj),
      timeout(timeoutMillis) {}

RemoteCommandRequest::RemoteCommandRequest(const HostAndPort& theTarget,
                                           const std::string& theDbName,
                                           const BSONObj& theCmdObj,
                                           const BSONObj& metadataObj,
                                           Milliseconds timeoutMillis)
    : RemoteCommandRequest(requestIdCounter.addAndFetch(1),
                           theTarget,
                           theDbName,
                           theCmdObj,
                           metadataObj,
                           timeoutMillis) {}

std::string RemoteCommandRequest::toString() const {
    str::stream out;
    out << "RemoteCommand " << id << " -- target:" << target.toString() << " db:" << dbname;

    if (expirationDate != kNoExpirationDate) {
        out << " expDate:" << expirationDate.toString();
    }

    out << " cmd:" << cmdObj.toString();
    return out;
}

bool RemoteCommandRequest::operator==(const RemoteCommandRequest& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return target == rhs.target && dbname == rhs.dbname && cmdObj == rhs.cmdObj &&
        metadata == rhs.metadata && timeout == rhs.timeout;
}

bool RemoteCommandRequest::operator!=(const RemoteCommandRequest& rhs) const {
    return !(*this == rhs);
}

}  // namespace executor

std::ostream& operator<<(std::ostream& os, const executor::RemoteCommandRequest& request) {
    return os << request.toString();
}

}  // namespace mongo
