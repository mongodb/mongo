/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/sessions_collection_rs.h"

#include <boost/optional.hpp>
#include <utility>

#include "mongo/client/connection_string.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

BSONObj lsidQuery(const LogicalSessionId& lsid) {
    return BSON(LogicalSessionRecord::kIdFieldName << lsid.toBSON());
}

Status makePrimaryConnection(OperationContext* opCtx, boost::optional<ScopedDbConnection>* conn) {
    auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
    auto config = coord->getConfig();
    if (!config.isInitialized()) {
        return {ErrorCodes::NotYetInitialized, "Replication has not yet been configured"};
    }

    // Find the primary
    RemoteCommandTargeterFactoryImpl factory;
    auto targeter = factory.create(config.getConnectionString());
    auto res = targeter->findHost(opCtx, ReadPreferenceSetting(ReadPreference::PrimaryOnly));
    if (!res.isOK()) {
        return res.getStatus();
    }

    auto hostname = res.getValue().toString();

    // Make a connection to the primary, auth, then send
    try {
        conn->emplace(hostname);
        if (isInternalAuthSet()) {
            (*conn)->get()->auth(getInternalUserAuthParams());
        }
        return Status::OK();
    } catch (...) {
        return exceptionToStatus();
    }
}

template <typename Callback>
Status runIfStandaloneOrPrimary(OperationContext* opCtx, Callback callback) {
    Lock::DBLock lk(opCtx, SessionsCollection::kSessionsDb, MODE_IX);
    Lock::CollectionLock lock(opCtx->lockState(), SessionsCollection::kSessionsFullNS, MODE_IX);

    auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
    if (coord->canAcceptWritesForDatabase(opCtx, SessionsCollection::kSessionsDb)) {
        return callback();
    }

    return {ErrorCodes::NotMaster, "Cannot perform a local write"};
}

}  // namespace

Status SessionsCollectionRS::refreshSessions(OperationContext* opCtx,
                                             const LogicalSessionRecordSet& sessions,
                                             Date_t refreshTime) {
    bool ran = false;

    // If we are the primary, write directly to ourself.
    auto status = runIfStandaloneOrPrimary(opCtx, [&] {
        ran = true;
        DBDirectClient client(opCtx);
        return doRefresh(sessions, refreshTime, makeSendFnForBatchWrite(&client));
    });

    if (ran) {
        return status;
    }

    // If we are not writeable, then send refreshSessions cmd to the primary.
    boost::optional<ScopedDbConnection> conn;
    auto res = makePrimaryConnection(opCtx, &conn);
    if (!res.isOK()) {
        return res;
    }

    return doRefreshExternal(sessions, refreshTime, makeSendFnForCommand(conn->get()));
}

Status SessionsCollectionRS::removeRecords(OperationContext* opCtx,
                                           const LogicalSessionIdSet& sessions) {
    bool ran = false;

    // If we are the primary, write directly to ourself.
    auto status = runIfStandaloneOrPrimary(opCtx, [&] {
        ran = true;
        DBDirectClient client(opCtx);
        return doRemove(sessions, makeSendFnForBatchWrite(&client));
    });

    if (ran) {
        return status;
    }

    // If we are not writeable, then send endSessions cmd to the primary
    boost::optional<ScopedDbConnection> conn;
    auto res = makePrimaryConnection(opCtx, &conn);
    if (!res.isOK()) {
        return res;
    }

    return doRemoveExternal(sessions, makeSendFnForCommand(conn->get()));
}


}  // namespace mongo
