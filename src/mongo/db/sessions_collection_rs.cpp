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

#include "mongo/platform/basic.h"

#include "mongo/db/sessions_collection_rs.h"

#include <boost/optional.hpp>
#include <memory>
#include <utility>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

namespace mongo {

auto SessionsCollectionRS::_makePrimaryConnection(OperationContext* opCtx) {
    // Find the primary
    if (stdx::lock_guard lk(_mutex); !_targeter) {
        // There is an assumption here that for the lifetime of a given process, the
        // ReplicationCoordiation will only return configs for a single replica set
        auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);
        auto config = coord->getConfig();
        uassert(ErrorCodes::NotYetInitialized,
                "Replication has not yet been configured",
                config.isInitialized());

        RemoteCommandTargeterFactoryImpl factory;
        _targeter = factory.create(config.getConnectionString());
    }

    auto res = uassertStatusOK(
        _targeter->findHost(opCtx, ReadPreferenceSetting(ReadPreference::PrimaryOnly)));

    auto conn = std::make_unique<ScopedDbConnection>(res.toString());

    // Make a connection to the primary, auth, then send
    if (auth::isInternalAuthSet()) {
        uassertStatusOK(conn->get()->authenticateInternalUser());
    }

    return conn;
}

bool SessionsCollectionRS::_isStandaloneOrPrimary(const NamespaceString& ns,
                                                  OperationContext* opCtx) {
    Lock::DBLock lk(opCtx, ns.db(), MODE_IS);
    Lock::CollectionLock lock(opCtx, NamespaceString::kLogicalSessionsNamespace, MODE_IS);

    auto coord = mongo::repl::ReplicationCoordinator::get(opCtx);

    return coord->canAcceptWritesForDatabase(opCtx, ns.db());
}

template <typename LocalCallback, typename RemoteCallback>
auto SessionsCollectionRS::_dispatch(const NamespaceString& ns,
                                     OperationContext* opCtx,
                                     LocalCallback&& localCallback,
                                     RemoteCallback&& remoteCallback)
    -> CommonResultT<LocalCallback, RemoteCallback> {
    if (_isStandaloneOrPrimary(ns, opCtx)) {
        return std::forward<LocalCallback>(localCallback)();
    }

    try {
        // There is a window here where we may transition from Primary to Secondary after we release
        // the locks we take in _isStandaloneOrPrimary(). In this case, the callback we run below
        // may throw a NotMaster error, or a stale read. However, this is preferable to running the
        // callback while we hold locks, since that can lead to a deadlock.

        auto conn = _makePrimaryConnection(opCtx);
        DBClientBase* client = conn->get();

        auto sosw = std::forward<RemoteCallback>(remoteCallback)(client);
        if (!sosw.isOK()) {
            conn->kill();
            return sosw;
        }

        conn->done();
        return sosw;
    } catch (...) {
        return exceptionToStatus();
    }
}

void SessionsCollectionRS::setupSessionsCollection(OperationContext* opCtx) {
    uassertStatusOKWithContext(
        _dispatch(
            NamespaceString::kLogicalSessionsNamespace,
            opCtx,
            [&] {
                auto existsStatus = checkSessionsCollectionExists(opCtx);
                if (existsStatus.isOK()) {
                    return Status::OK();
                }

                DBDirectClient client(opCtx);
                BSONObj cmd;

                if (existsStatus.code() == ErrorCodes::IndexOptionsConflict) {
                    cmd = generateCollModCmd();
                } else {
                    // Creating the TTL index will auto-generate the collection.
                    cmd = generateCreateIndexesCmd();
                }

                BSONObj info;
                if (!client.runCommand(
                        NamespaceString::kLogicalSessionsNamespace.db().toString(), cmd, info)) {
                    return getStatusFromCommandResult(info);
                }

                return Status::OK();
            },
            [&](DBClientBase*) { return checkSessionsCollectionExists(opCtx); }),
        str::stream() << "Failed to create " << NamespaceString::kLogicalSessionsNamespace);
}

Status SessionsCollectionRS::checkSessionsCollectionExists(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    auto indexes = client.getIndexSpecs(NamespaceString::kLogicalSessionsNamespace);

    if (indexes.size() == 0u) {
        return Status{ErrorCodes::NamespaceNotFound, "config.system.sessions does not exist"};
    }

    auto index = std::find_if(indexes.begin(), indexes.end(), [](const BSONObj& index) {
        return index.getField("name").String() == kSessionsTTLIndex;
    });

    if (index == indexes.end()) {
        return Status{ErrorCodes::IndexNotFound,
                      "config.system.sessions does not have the required TTL index"};
    }

    if (!index->hasField("expireAfterSeconds") ||
        index->getField("expireAfterSeconds").Int() != (localLogicalSessionTimeoutMinutes * 60)) {
        return Status{
            ErrorCodes::IndexOptionsConflict,
            "config.system.sessions currently has the incorrect timeout for the TTL index"};
    }

    return Status::OK();
}

Status SessionsCollectionRS::refreshSessions(OperationContext* opCtx,
                                             const LogicalSessionRecordSet& sessions) {
    const std::vector<LogicalSessionRecord> sessionsVector(sessions.begin(), sessions.end());

    return _dispatch(NamespaceString::kLogicalSessionsNamespace,
                     opCtx,
                     [&] {
                         DBDirectClient client(opCtx);
                         return doRefresh(NamespaceString::kLogicalSessionsNamespace,
                                          sessionsVector,
                                          makeSendFnForBatchWrite(
                                              NamespaceString::kLogicalSessionsNamespace, &client));
                     },
                     [&](DBClientBase* client) {
                         return doRefresh(NamespaceString::kLogicalSessionsNamespace,
                                          sessionsVector,
                                          makeSendFnForBatchWrite(
                                              NamespaceString::kLogicalSessionsNamespace, client));
                     });
}

Status SessionsCollectionRS::removeRecords(OperationContext* opCtx,
                                           const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());

    return _dispatch(NamespaceString::kLogicalSessionsNamespace,
                     opCtx,
                     [&] {
                         DBDirectClient client(opCtx);
                         return doRemove(NamespaceString::kLogicalSessionsNamespace,
                                         sessionsVector,
                                         makeSendFnForBatchWrite(
                                             NamespaceString::kLogicalSessionsNamespace, &client));
                     },
                     [&](DBClientBase* client) {
                         return doRemove(NamespaceString::kLogicalSessionsNamespace,
                                         sessionsVector,
                                         makeSendFnForBatchWrite(
                                             NamespaceString::kLogicalSessionsNamespace, client));
                     });
}

StatusWith<LogicalSessionIdSet> SessionsCollectionRS::findRemovedSessions(
    OperationContext* opCtx, const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());

    return _dispatch(
        NamespaceString::kLogicalSessionsNamespace,
        opCtx,
        [&] {
            DBDirectClient client(opCtx);
            return doFindRemoved(
                NamespaceString::kLogicalSessionsNamespace,
                sessionsVector,
                makeFindFnForCommand(NamespaceString::kLogicalSessionsNamespace, &client));
        },
        [&](DBClientBase* client) {
            return doFindRemoved(
                NamespaceString::kLogicalSessionsNamespace,
                sessionsVector,
                makeFindFnForCommand(NamespaceString::kLogicalSessionsNamespace, client));
        });
}

}  // namespace mongo
