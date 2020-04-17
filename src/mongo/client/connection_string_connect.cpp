/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"

#include <list>
#include <memory>

#include "mongo/client/dbclient_rs.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {

Mutex ConnectionString::_connectHookMutex = MONGO_MAKE_LATCH();
ConnectionString::ConnectionHook* ConnectionString::_connectHook = nullptr;

std::unique_ptr<DBClientBase> ConnectionString::connect(StringData applicationName,
                                                        std::string& errmsg,
                                                        double socketTimeout,
                                                        const MongoURI* uri) const {
    MongoURI newURI{};
    if (uri) {
        newURI = *uri;
    }

    switch (_type) {
        case MASTER: {
            for (const auto& server : _servers) {
                auto c = std::make_unique<DBClientConnection>(true, 0, newURI);

                c->setSoTimeout(socketTimeout);
                LOGV2_DEBUG(20109,
                            1,
                            "Creating new connection to: {hostAndPort}",
                            "Creating new connection",
                            "hostAndPort"_attr = server);
                if (!c->connect(server, applicationName, errmsg)) {
                    continue;
                }
                LOGV2_DEBUG(20110, 1, "Connected connection!");
                return std::move(c);
            }
            return nullptr;
        }

        case SET: {
            auto set = std::make_unique<DBClientReplicaSet>(
                _setName, _servers, applicationName, socketTimeout, std::move(newURI));
            if (!set->connect()) {
                errmsg = "connect failed to replica set ";
                errmsg += toString();
                return nullptr;
            }
            return std::move(set);
        }

        case CUSTOM: {
            // Lock in case other things are modifying this at the same time
            stdx::lock_guard<Latch> lk(_connectHookMutex);

            // Allow the replacement of connections with other connections - useful for testing.

            uassert(16335,
                    "custom connection to " + this->toString() +
                        " specified with no connection hook",
                    _connectHook);

            // Double-checked lock, since this will never be active during normal operation
            auto replacementConn = _connectHook->connect(*this, errmsg, socketTimeout);

            LOGV2(20111,
                  "Replacing connection to {oldConnString} with {newConnString}",
                  "Replacing connection string",
                  "oldConnString"_attr = this->toString(),
                  "newConnString"_attr =
                      (replacementConn ? replacementConn->getServerAddress() : "(empty)"));

            return replacementConn;
        }

        case LOCAL:
        case INVALID:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
