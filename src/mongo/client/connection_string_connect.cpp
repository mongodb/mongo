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


#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/client/client_api_version_parameters_gen.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

Mutex ConnectionString::_connectHookMutex = MONGO_MAKE_LATCH();
ConnectionString::ConnectionHook* ConnectionString::_connectHook = nullptr;

StatusWith<std::unique_ptr<DBClientBase>> ConnectionString::connect(
    StringData applicationName,
    double socketTimeout,
    const MongoURI* uri,
    const ClientAPIVersionParameters* apiParameters,
    const TransientSSLParams* transientSSLParams) const {
    MongoURI newURI{};
    if (uri) {
        newURI = *uri;
    }

    switch (_type) {
        case ConnectionType::kStandalone: {
            Status lastError =
                Status(ErrorCodes::BadValue,
                       "Invalid standalone connection string with empty server list.");
            for (const auto& server : _servers) {
                auto c = std::make_unique<DBClientConnection>(
                    true, 0, newURI, DBClientConnection::HandshakeValidationHook(), apiParameters);

                c->setSoTimeout(socketTimeout);
                LOGV2_DEBUG(20109,
                            1,
                            "Creating new connection",
                            "hostAndPort"_attr = server,
                            "gRPC"_attr = newURI.isGRPC());
                try {
                    c->connect(server,
                               applicationName,
                               transientSSLParams ? boost::make_optional(*transientSSLParams)
                                                  : boost::none);
                } catch (const DBException& e) {
                    lastError = e.toStatus();
                    continue;
                }

#ifdef MONGO_CONFIG_SSL
                invariant((transientSSLParams != nullptr) == c->isUsingTransientSSLParams());
#endif
                LOGV2_DEBUG(20110, 1, "Connected connection!");
                return std::move(c);
            }
            return lastError;
        }

        case ConnectionType::kReplicaSet: {
            auto set = std::make_unique<DBClientReplicaSet>(_replicaSetName,
                                                            _servers,
                                                            applicationName,
                                                            socketTimeout,
                                                            std::move(newURI),
                                                            apiParameters);
            auto status = set->connect();
            if (!status.isOK()) {
                return status.withReason(status.reason() + ", " + toString());
            }

#ifdef MONGO_CONFIG_SSL
            invariant(!set->isUsingTransientSSLParams());  // Not implemented.
#endif
            return std::move(set);
        }

        case ConnectionType::kCustom: {
            // Lock in case other things are modifying this at the same time
            stdx::lock_guard<Latch> lk(_connectHookMutex);

            // Allow the replacement of connections with other connections - useful for testing.

            uassert(16335,
                    "custom connection to " + this->toString() +
                        " specified with no connection hook",
                    _connectHook);

            // Double-checked lock, since this will never be active during normal operation
            std::string errmsg;
            auto replacementConn =
                _connectHook->connect(*this, errmsg, socketTimeout, apiParameters);

            LOGV2(20111,
                  "Replacing connection to {oldConnString} with {newConnString}",
                  "Replacing connection string",
                  "oldConnString"_attr = this->toString(),
                  "newConnString"_attr =
                      (replacementConn ? replacementConn->getServerAddress() : "(empty)"));

            if (replacementConn) {
                return std::move(replacementConn);
            }
            return Status(ErrorCodes::HostUnreachable, "Connection hook error: " + errmsg);
        }

        case ConnectionType::kLocal:
        case ConnectionType::kInvalid:
            MONGO_UNREACHABLE;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
