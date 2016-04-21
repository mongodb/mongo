/**
 *    Copyright (C) 2008 MongoDB Inc.
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

}  // namespace


void profile(OperationContext* txn, NetworkOp op) {
    // Initialize with 1kb at start in order to avoid realloc later
    BufBuilder profileBufBuilder(1024);

    BSONObjBuilder b(profileBufBuilder);

    {
        Locker::LockerInfo lockerInfo;
        txn->lockState()->getLockerInfo(&lockerInfo);
        CurOp::get(txn)->debug().append(*CurOp::get(txn), lockerInfo.stats, b);
    }

    b.appendDate("ts", jsTime());
    b.append("client", txn->getClient()->clientAddress());

    AuthorizationSession* authSession = AuthorizationSession::get(txn->getClient());
    _appendUserInfo(*CurOp::get(txn), b, authSession);

    const BSONObj p = b.done();

    const bool wasLocked = txn->lockState()->isLocked();

    const string dbName(nsToDatabase(CurOp::get(txn)->getNS()));

    try {
        bool acquireDbXLock = false;
        while (true) {
            ScopedTransaction scopedXact(txn, MODE_IX);

            std::unique_ptr<AutoGetDb> autoGetDb;
            if (acquireDbXLock) {
                autoGetDb.reset(new AutoGetDb(txn, dbName, MODE_X));
                if (autoGetDb->getDb()) {
                    createProfileCollection(txn, autoGetDb->getDb());
                }
            } else {
                autoGetDb.reset(new AutoGetDb(txn, dbName, MODE_IX));
            }

            Database* const db = autoGetDb->getDb();
            if (!db) {
                // Database disappeared
                log() << "note: not profiling because db went away for "
                      << CurOp::get(txn)->getNS();
                break;
            }

            Lock::CollectionLock collLock(txn->lockState(), db->getProfilingNS(), MODE_IX);

            Collection* const coll = db->getCollection(db->getProfilingNS());
            if (coll) {
                WriteUnitOfWork wuow(txn);
                OpDebug* const nullOpDebug = nullptr;
                coll->insertDocument(txn, p, nullOpDebug, false);
                wuow.commit();

                break;
            } else if (!acquireDbXLock &&
                       (!wasLocked || txn->lockState()->isDbLockedForMode(dbName, MODE_X))) {
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
        warning() << "Caught Assertion while trying to profile " << networkOpToString(op)
                  << " against " << CurOp::get(txn)->getNS() << ": " << assertionEx.toString()
                  << endl;
    }
}


Status createProfileCollection(OperationContext* txn, Database* db) {
    invariant(txn->lockState()->isDbLockedForMode(db->name(), MODE_X));

    const std::string dbProfilingNS(db->getProfilingNS());

    Collection* const collection = db->getCollection(dbProfilingNS);
    if (collection) {
        if (!collection->isCapped()) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << dbProfilingNS << " exists but isn't capped");
        }

        return Status::OK();
    }

    // system.profile namespace doesn't exist; create it
    log() << "Creating profile collection: " << dbProfilingNS << endl;

    CollectionOptions collectionOptions;
    collectionOptions.capped = true;
    collectionOptions.cappedSize = 1024 * 1024;

    WriteUnitOfWork wunit(txn);
    bool shouldReplicateWrites = txn->writesAreReplicated();
    txn->setReplicatedWrites(false);
    ON_BLOCK_EXIT(&OperationContext::setReplicatedWrites, txn, shouldReplicateWrites);
    invariant(db->createCollection(txn, dbProfilingNS, collectionOptions));
    wunit.commit();

    return Status::OK();
}

}  // namespace mongo
