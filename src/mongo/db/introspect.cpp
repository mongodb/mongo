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
#include "mongo/rpc/metadata/client_metadata_ismaster.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::string;

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

/**
 * When in scope, closes any active storage transactions and enforces prepare conflicts for reads.
 *
 * Locks must be held while this is in scope because both constructor and destructor access the
 * storage engine.
 */
class EnforcePrepareConflictsBlock {
public:
    explicit EnforcePrepareConflictsBlock(OperationContext* opCtx)
        : _opCtx(opCtx), _originalValue(opCtx->recoveryUnit()->getIgnorePrepared()) {
        dassert(_opCtx->lockState()->isLocked());
        dassert(!_opCtx->lockState()->inAWriteUnitOfWork());

        // It is illegal to call setIgnorePrepared() while any storage transaction is active. This
        // call is also harmless because any previous reads or writes should have already completed,
        // as profile() is called at the end of an operation.
        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setIgnorePrepared(false);
    }

    ~EnforcePrepareConflictsBlock() {
        dassert(_opCtx->lockState()->isLocked());
        dassert(!_opCtx->lockState()->inAWriteUnitOfWork());

        _opCtx->recoveryUnit()->abandonSnapshot();
        _opCtx->recoveryUnit()->setIgnorePrepared(_originalValue);
    }

private:
    OperationContext* _opCtx;
    bool _originalValue;
};

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

    const bool wasLocked = opCtx->lockState()->isLocked();

    const string dbName(nsToDatabase(CurOp::get(opCtx)->getNS()));

    // True if we need to acquire an X lock on the database in order to create the system.profile
    // collection.
    bool acquireDbXLock = false;

    try {
        // Even if the operation we are profiling was interrupted, we still want to output the
        // profiler entry.  This lock guard will prevent lock acquisitions from throwing exceptions
        // before we finish writing the entry. However, our maximum lock timeout overrides
        // uninterruptibility.
        boost::optional<UninterruptibleLockGuard> noInterrupt;
        if (!opCtx->lockState()->hasMaxLockTimeout()) {
            noInterrupt.emplace(opCtx->lockState());
        }

        while (true) {
            std::unique_ptr<AutoGetDb> autoGetDb;
            if (acquireDbXLock) {
                // We should not attempt to acquire an X lock while in "noInterrupt" scope.
                noInterrupt.reset();

                autoGetDb.reset(new AutoGetDb(opCtx, dbName, MODE_X));
                if (autoGetDb->getDb()) {
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
    invariant(db->createCollection(opCtx, dbProfilingNS.ns(), collectionOptions));
    wunit.commit();

    return Status::OK();
}

}  // namespace mongo
