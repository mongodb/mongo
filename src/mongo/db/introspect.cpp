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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/introspect.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_set.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::endl;
using std::string;
using std::unique_ptr;

namespace {

void _appendUserInfo(const CurOp& c, BSONObjBuilder& builder, AuthorizationSession* authSession) {
    UserNameIterator nameIter = authSession->getAuthenticatedUserNames();

    UserName bestUser;
    if (nameIter.more())
        bestUser = *nameIter;

    std::string opdb(nsToDatabase(c.getNS()));

    BSONArrayBuilder allUsers(builder.subarrayStart("allUsers"));
    for (; nameIter.more(); nameIter.next()) {
        BSONObjBuilder nextUser(allUsers.subobjStart());
        nextUser.append(AuthorizationManager::USER_NAME_FIELD_NAME, nameIter->getUser());
        nextUser.append(AuthorizationManager::USER_DB_FIELD_NAME, nameIter->getDB());
        nextUser.doneFast();

        if (nameIter->getDB() == opdb) {
            bestUser = *nameIter;
        }
    }
    allUsers.doneFast();

    builder.append("user", bestUser.getUser().empty() ? "" : bestUser.getFullName());
}

}  // namespace


void profile(OperationContext* opCtx, NetworkOp op) {
    // Initialize with 1kb at start in order to avoid realloc later
    BufBuilder profileBufBuilder(1024);

    BSONObjBuilder b(profileBufBuilder);

    {
        Locker::LockerInfo lockerInfo;
        opCtx->lockState()->getLockerInfo(&lockerInfo, CurOp::get(opCtx)->getLockStatsBase());
        CurOp::get(opCtx)->debug().append(
            opCtx, lockerInfo.stats, opCtx->lockState()->getFlowControlStats(), b);
    }

    b.appendDate("ts", jsTime());
    b.append("client", opCtx->getClient()->clientAddress());

    const auto& clientMetadata =
        ClientMetadataIsMasterState::get(opCtx->getClient()).getClientMetadata();
    if (clientMetadata) {
        auto appName = clientMetadata.get().getApplicationName();
        if (!appName.empty()) {
            b.append("appName", appName);
        }
    }

    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    _appendUserInfo(*CurOp::get(opCtx), b, authSession);

    const BSONObj p = b.done();

    const string dbName(nsToDatabase(CurOp::get(opCtx)->getNS()));

    auto origFlowControl = opCtx->shouldParticipateInFlowControl();

    // The system.profile collection is non-replicated, so writes to it do not cause
    // replication lag. As such, they should be excluded from Flow Control.
    opCtx->setShouldParticipateInFlowControl(false);

    // IX lock acquisitions beyond this block will not be related to writes to system.profile.
    ON_BLOCK_EXIT(
        [opCtx, origFlowControl] { opCtx->setShouldParticipateInFlowControl(origFlowControl); });

    try {

        // Even if the operation we are profiling was interrupted, we still want to output the
        // profiler entry.  This lock guard will prevent lock acquisitions from throwing exceptions
        // before we finish writing the entry. However, our maximum lock timeout overrides
        // uninterruptibility.
        boost::optional<UninterruptibleLockGuard> noInterrupt;
        if (!opCtx->lockState()->hasMaxLockTimeout()) {
            noInterrupt.emplace(opCtx->lockState());
        }

        const auto dbProfilingNS = NamespaceString(dbName, "system.profile");
        AutoGetCollection autoColl(opCtx, dbProfilingNS, MODE_IX);
        Database* const db = autoColl.getDb();
        if (!db) {
            // Database disappeared.
            LOGV2(20700,
                  "note: not profiling because db went away for {namespace}",
                  "note: not profiling because db went away for namespace",
                  "namespace"_attr = CurOp::get(opCtx)->getNS());
            return;
        }

        // We are about to enforce prepare conflicts for the OperationContext. But it is illegal
        // to change the behavior of ignoring prepare conflicts while any storage transaction is
        // still active. So we need to call abandonSnapshot() to close any open transactions.
        // This call is also harmless because any previous reads or writes should have already
        // completed, as profile() is called at the end of an operation.
        opCtx->recoveryUnit()->abandonSnapshot();
        // The profiler performs writes even after read commands. Ignoring prepare conflicts is
        // not allowed while performing writes, so temporarily enforce prepare conflicts.
        EnforcePrepareConflictsBlock enforcePrepare(opCtx);

        uassertStatusOK(createProfileCollection(opCtx, db));
        Collection* const coll =
            CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, dbProfilingNS);

        invariant(!opCtx->shouldParticipateInFlowControl());
        WriteUnitOfWork wuow(opCtx);
        OpDebug* const nullOpDebug = nullptr;
        uassertStatusOK(coll->insertDocument(opCtx, InsertStatement(p), nullOpDebug, false));
        wuow.commit();
    } catch (const AssertionException& assertionEx) {
        LOGV2_WARNING(20703,
                      "Caught Assertion while trying to profile {operation} against "
                      "{namespace}: {assertion}",
                      "Caught Assertion while trying to profile operation",
                      "operation"_attr = networkOpToString(op),
                      "namespace"_attr = CurOp::get(opCtx)->getNS(),
                      "assertion"_attr = redact(assertionEx));
    }
}


Status createProfileCollection(OperationContext* opCtx, Database* db) {
    invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_IX));
    invariant(!opCtx->shouldParticipateInFlowControl());

    const auto dbProfilingNS = NamespaceString(db->name(), "system.profile");
    Collection* const collection =
        CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, dbProfilingNS);
    if (collection) {
        if (!collection->isCapped()) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << dbProfilingNS << " exists but isn't capped");
        }

        return Status::OK();
    }

    // system.profile namespace doesn't exist; create it
    LOGV2(20701,
          "Creating profile collection: {dbProfilingNS}",
          "dbProfilingNS"_attr = dbProfilingNS);

    CollectionOptions collectionOptions;
    collectionOptions.capped = true;
    collectionOptions.cappedSize = 1024 * 1024;

    WriteUnitOfWork wunit(opCtx);
    repl::UnreplicatedWritesBlock uwb(opCtx);
    invariant(db->createCollection(opCtx, dbProfilingNS, collectionOptions));
    wunit.commit();

    return Status::OK();
}

}  // namespace mongo
