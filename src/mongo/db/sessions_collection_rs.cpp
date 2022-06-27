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
    ShouldNotConflictWithSecondaryBatchApplicationBlock noPBWMBlock(opCtx->lockState());
    Lock::DBLock lk(opCtx, ns.dbName(), MODE_IS);
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

    // There is a window here where we may transition from Primary to Secondary after we release
    // the locks we take in _isStandaloneOrPrimary(). In this case, the callback we run below
    // may throw a NotWritablePrimary error, or a stale read. However, this is preferable to running
    // the callback while we hold locks, since that can lead to a deadlock.

    auto conn = _makePrimaryConnection(opCtx);
    DBClientBase* client = conn->get();
    ScopeGuard guard([&] { conn->done(); });
    try {
        return std::forward<RemoteCallback>(remoteCallback)(client);
    } catch (...) {
        guard.dismiss();
        conn->kill();
        throw;
    }
}

void SessionsCollectionRS::setupSessionsCollection(OperationContext* opCtx) {
    _dispatch(
        NamespaceString::kLogicalSessionsNamespace,
        opCtx,
        [&] {
            try {
                checkSessionsCollectionExists(opCtx);
            } catch (const DBException& ex) {

                DBDirectClient client(opCtx);
                BSONObj cmd;

                if (ex.code() == ErrorCodes::IndexOptionsConflict) {
                    cmd = generateCollModCmd();
                } else {
                    // Creating the TTL index will auto-generate the collection.
                    cmd = generateCreateIndexesCmd();
                }

                BSONObj info;
                if (!client.runCommand(
                        NamespaceString::kLogicalSessionsNamespace.db().toString(), cmd, info)) {
                    uassertStatusOK(getStatusFromCommandResult(info));
                }
            }
        },
        [&](DBClientBase*) { checkSessionsCollectionExists(opCtx); });
}

void SessionsCollectionRS::checkSessionsCollectionExists(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    const bool includeBuildUUIDs = false;
    const int options = 0;
    auto indexes = client.getIndexSpecs(
        NamespaceString::kLogicalSessionsNamespace, includeBuildUUIDs, options);

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << NamespaceString::kLogicalSessionsNamespace << " does not exist",
            indexes.size() != 0u);

    auto index = std::find_if(indexes.begin(), indexes.end(), [](const BSONObj& index) {
        return index.getField("name").String() == kSessionsTTLIndex;
    });

    uassert(ErrorCodes::IndexNotFound,
            str::stream() << NamespaceString::kLogicalSessionsNamespace
                          << " does not have the required TTL index",
            index != indexes.end());

    uassert(ErrorCodes::IndexOptionsConflict,
            str::stream() << NamespaceString::kLogicalSessionsNamespace
                          << " currently has the incorrect timeout for the TTL index",
            index->hasField("expireAfterSeconds") &&
                index->getField("expireAfterSeconds").Int() ==
                    (localLogicalSessionTimeoutMinutes * 60));
}

void SessionsCollectionRS::refreshSessions(OperationContext* opCtx,
                                           const LogicalSessionRecordSet& sessions) {
    const std::vector<LogicalSessionRecord> sessionsVector(sessions.begin(), sessions.end());

    _dispatch(NamespaceString::kLogicalSessionsNamespace,
              opCtx,
              [&] {
                  DBDirectClient client(opCtx);
                  _doRefresh(
                      NamespaceString::kLogicalSessionsNamespace,
                      sessionsVector,
                      makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, &client));
              },
              [&](DBClientBase* client) {
                  _doRefresh(
                      NamespaceString::kLogicalSessionsNamespace,
                      sessionsVector,
                      makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, client));
              });
}

void SessionsCollectionRS::removeRecords(OperationContext* opCtx,
                                         const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());

    _dispatch(
        NamespaceString::kLogicalSessionsNamespace,
        opCtx,
        [&] {
            DBDirectClient client(opCtx);
            _doRemove(NamespaceString::kLogicalSessionsNamespace,
                      sessionsVector,
                      makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, &client));
        },
        [&](DBClientBase* client) {
            _doRemove(NamespaceString::kLogicalSessionsNamespace,
                      sessionsVector,
                      makeSendFnForBatchWrite(NamespaceString::kLogicalSessionsNamespace, client));
        });
}

LogicalSessionIdSet SessionsCollectionRS::findRemovedSessions(OperationContext* opCtx,
                                                              const LogicalSessionIdSet& sessions) {
    const std::vector<LogicalSessionId> sessionsVector(sessions.begin(), sessions.end());

    return _dispatch(
        NamespaceString::kLogicalSessionsNamespace,
        opCtx,
        [&] {
            DBDirectClient client(opCtx);
            return _doFindRemoved(
                NamespaceString::kLogicalSessionsNamespace,
                sessionsVector,
                makeFindFnForCommand(NamespaceString::kLogicalSessionsNamespace, &client));
        },
        [&](DBClientBase* client) {
            return _doFindRemoved(
                NamespaceString::kLogicalSessionsNamespace,
                sessionsVector,
                makeFindFnForCommand(NamespaceString::kLogicalSessionsNamespace, client));
        });
}

}  // namespace mongo
