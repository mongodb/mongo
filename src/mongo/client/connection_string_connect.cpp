/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"

#include <list>
#include <memory>

#include "mongo/client/dbclient_rs.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

stdx::mutex ConnectionString::_connectHookMutex;
ConnectionString::ConnectionHook* ConnectionString::_connectHook = NULL;

DBClientBase* ConnectionString::connect(std::string& errmsg, double socketTimeout) const {
    switch (_type) {
        case MASTER: {
            auto c = stdx::make_unique<DBClientConnection>(true);
            c->setSoTimeout(socketTimeout);
            LOG(1) << "creating new connection to:" << _servers[0];
            if (!c->connect(_servers[0], errmsg)) {
                return 0;
            }
            LOG(1) << "connected connection!";
            return c.release();
        }

        case SET: {
            auto set = stdx::make_unique<DBClientReplicaSet>(_setName, _servers, socketTimeout);
            if (!set->connect()) {
                errmsg = "connect failed to replica set ";
                errmsg += toString();
                return 0;
            }
            return set.release();
        }

        case CUSTOM: {
            // Lock in case other things are modifying this at the same time
            stdx::lock_guard<stdx::mutex> lk(_connectHookMutex);

            // Allow the replacement of connections with other connections - useful for testing.

            uassert(16335,
                    "custom connection to " + this->toString() +
                        " specified with no connection hook",
                    _connectHook);

            // Double-checked lock, since this will never be active during normal operation
            DBClientBase* replacementConn = _connectHook->connect(*this, errmsg, socketTimeout);

            log() << "replacing connection to " << this->toString() << " with "
                  << (replacementConn ? replacementConn->getServerAddress() : "(empty)");

            return replacementConn;
        }

        case LOCAL:
        case INVALID:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

}  // namepspace mongo
