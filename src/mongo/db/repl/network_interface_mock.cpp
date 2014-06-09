/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/network_interface_mock.h"

#include <map>

#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/map_util.h"

namespace mongo {
namespace repl {

    bool operator<(const ReplicationExecutor::RemoteCommandRequest& lhs,
                   const ReplicationExecutor::RemoteCommandRequest& rhs) {
        if (lhs.target < rhs.target)
            return true;
        if (rhs.target < lhs.target)
            return false;
        if (lhs.dbname < rhs.dbname)
            return true;
        if (rhs.dbname < lhs.dbname)
            return false;
        return lhs.cmdObj < rhs.cmdObj;
    }

    Date_t NetworkInterfaceMock::now() {
        return curTimeMillis64();
    }

    StatusWith<BSONObj> NetworkInterfaceMock::runCommand(
            const ReplicationExecutor::RemoteCommandRequest& request) {
        return mapFindWithDefault(
                _responses,
                request,
                StatusWith<BSONObj>(
                        ErrorCodes::NoSuchKey,
                        mongoutils::str::stream() << "Could not find response for " <<
                                "Request(" << request.target.toString() << ", " <<
                                request.dbname << ", " << request.cmdObj << ')'));
    }

    void NetworkInterfaceMock::runCallbackWithGlobalExclusiveLock(
            const stdx::function<void ()>& callback) {

        callback();
    }

    bool NetworkInterfaceMock::addResponse(
            const ReplicationExecutor::RemoteCommandRequest& request,
            const StatusWith<BSONObj>& response) {

        return _responses.insert(std::make_pair(request, response)).second;
    }

}  // namespace repl
}  // namespace mongo
