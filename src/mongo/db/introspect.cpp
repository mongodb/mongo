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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

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
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/log.h"
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
            *CurOp::get(opCtx), lockerInfo.stats, opCtx->lockState()->getFlowControlStats(), b);
    }

    b.appendDate("ts", jsTime());
    b.append("client", opCtx->getClient()->clientAddress());

    if (auto clientMetadata = ClientMetadata::get(opCtx->getClient())) {
        auto appName = clientMetadata->getApplicationName();
        if (!appName.empty()) {
            b.append("appName", appName);
        }
    }

    AuthorizationSession* authSession = AuthorizationSession::get(opCtx->getClient());
    _appendUserInfo(*CurOp::get(opCtx), b, authSession);

    const BSONObj p = b.done();

    const bool wasLocked = opCtx->lockState()->isLocked();

    const string dbName(nsToDatabase(CurOp::get(opCtx)->getNS()));

    // True if we need to acquire an X lock on the database in order to create the system.profile
    // collection.
    bool acquireDbXLock = false;

    try {
        // Set the opCtx to be only interruptible for replication state changes. This is needed
        // because for some operations that are already marked as killed due to errors such as
        // operation time exceeding maxTimeMS, we still want to output the profiler entry. Thus
        // in these cases we do not interrupt lock acquisition even though the opCtx has already
        // been killed. In the meantime we need to make sure replication state changes can still
        // interrupt lock acquisition, otherwise there could be deadlocks when the state change
        // thread is waiting for the session checked out by this opCtx while holding RSTL lock.
        // However when maxLockTimeout is set, we want it to be always interruptible.
        if (!opCtx->lockState()->hasMaxLockTimeout()) {
            opCtx->setIgnoreInterruptsExceptForReplStateChange(true);
        }

        // IX lock acquisitions beyond this block will not be related to writes to system.profile.
        ON_BLOCK_EXIT([opCtx] { opCtx->setIgnoreInterruptsExceptForReplStateChange(false); });

        while (true) {
            std::unique_ptr<AutoGetDb> autoGetDb;
            if (acquireDbXLock) {
                // We should not attempt to acquire an X lock while opCtx ignores interrupts.
                opCtx->setIgnoreInterruptsExceptForReplStateChange(false);

                autoGetDb.reset(new AutoGetDb(opCtx, dbName, MODE_X));
                if (autoGetDb->getDb()) {
                    // We are about to enforce prepare conflicts for the OperationContext. But it is
                    // illegal to change the behavior of ignoring prepare conflicts while any
                    // storage transaction is still active. So we need to call abandonSnapshot() to
                    // close any open transactions. This call is also harmless because any previous
                    // reads or writes should have already completed, as profile() is called at the
                    // end of an operation.
                    opCtx->recoveryUnit()->abandonSnapshot();
                    // The profiler performs writes even after read commands. Ignoring prepare
                    // conflicts is not allowed while performing writes, so temporarily enforce
                    // prepare conflicts.
                    EnforcePrepareConflictsBlock enforcePrepare(opCtx);
                    createProfileCollection(opCtx, autoGetDb->getDb()).transitional_ignore();
                }
            } else {
                autoGetDb.reset(new AutoGetDb(opCtx, dbName, MODE_IX));
            }

            Database* const db = autoGetDb->getDb();
            if (!db) {
                // Database disappeared
                log() << "note: not profiling because db went away for "
                      << CurOp::get(opCtx)->getNS();
                break;
            }

            Lock::CollectionLock collLock(opCtx, db->getProfilingNS(), MODE_IX);

            // We are about to enforce prepare conflicts for the OperationContext. But it is illegal
            // to change the behavior of ignoring prepare conflicts while any storage transaction is
            // still active. So we need to call abandonSnapshot() to close any open transactions.
            // This call is also harmless because any previous reads or writes should have already
            // completed, as profile() is called at the end of an operation.
            opCtx->recoveryUnit()->abandonSnapshot();
            // The profiler performs writes even after read commands. Ignoring prepare conflicts is
            // not allowed while performing writes, so temporarily enforce prepare conflicts.
            EnforcePrepareConflictsBlock enforcePrepare(opCtx);

            Collection* const coll = db->getCollection(opCtx, db->getProfilingNS());
            if (coll) {
                WriteUnitOfWork wuow(opCtx);
                OpDebug* const nullOpDebug = nullptr;
                coll->insertDocument(opCtx, InsertStatement(p), nullOpDebug, false)
                    .transitional_ignore();
                wuow.commit();

                break;
            } else if (!acquireDbXLock &&
                       (!wasLocked || opCtx->lockState()->isDbLockedForMode(dbName, MODE_X))) {
                // Try to create the collection only if we are not under lock, in order to
                // avoid deadlocks due to lock conversion. This would only be hit if someone
                // deletes the profiler collection after setting profile level.
                acquireDbXLock = true;
            } else {
                // Cannot write the profile information
                break;
            }
        }
    } catch (const AssertionException& assertionEx) {
        if (acquireDbXLock && assertionEx.isA<ErrorCategory::Interruption>()) {
            warning()
                << "Interrupted while attempting to create profile collection in database "
                << dbName << " to profile operation " << networkOpToString(op) << " against "
                << CurOp::get(opCtx)->getNS()
                << ". Manually create profile collection to ensure future operations are logged.";
        } else {
            warning() << "Caught Assertion while trying to profile " << networkOpToString(op)
                      << " against " << CurOp::get(opCtx)->getNS() << ": " << redact(assertionEx);
        }
    }
}


Status createProfileCollection(OperationContext* opCtx, Database* db) {
    invariant(opCtx->lockState()->isDbLockedForMode(db->name(), MODE_X));

    auto& dbProfilingNS = db->getProfilingNS();
    Collection* const collection = db->getCollection(opCtx, dbProfilingNS);
    if (collection) {
        if (!collection->isCapped()) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << dbProfilingNS << " exists but isn't capped");
        }

        return Status::OK();
    }

    // system.profile namespace doesn't exist; create it
    log() << "Creating profile collection: " << dbProfilingNS;

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
