// introspect.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED

#include <boost/scoped_ptr.hpp>

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_set.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::endl;
    using std::string;

namespace {
    void _appendUserInfo(const CurOp& c,
                         BSONObjBuilder& builder,
                         AuthorizationSession* authSession) {
        UserNameIterator nameIter = authSession->getAuthenticatedUserNames();

        UserName bestUser;
        if (nameIter.more())
            bestUser = *nameIter;

        std::string opdb( nsToDatabase( c.getNS() ) );

        BSONArrayBuilder allUsers(builder.subarrayStart("allUsers"));
        for ( ; nameIter.more(); nameIter.next()) {
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
} // namespace

    /**
     * @return if collection existed or was created
     */
    static bool _profile(OperationContext* txn,
                         const Client& c,
                         Database* db,
                         CurOp& currentOp,
                         BufBuilder& profileBufBuilder) {
        dassert( db );

        // build object
        BSONObjBuilder b(profileBufBuilder);

        currentOp.debug().append(currentOp, b);

        b.appendDate("ts", jsTime());
        b.append("client", c.clientAddress());

        AuthorizationSession * authSession = c.getAuthorizationSession();
        _appendUserInfo(currentOp, b, authSession);

        BSONObj p = b.done();

        WriteUnitOfWork wunit(txn);

        // write: not replicated
        // get or create the profiling collection
        Collection* profileCollection = getOrCreateProfileCollection(txn, db);
        if ( !profileCollection ) {
            return false;
        }
        profileCollection->insertDocument( txn, p, false );
        wunit.commit();
        return true;
    }

    void profile(OperationContext* txn, const Client& c, int op, CurOp& currentOp) {
        bool tryAgain = false;
        while ( 1 ) {
            try {
                // initialize with 1kb to start, to avoid realloc later
                // doing this outside the dblock to improve performance
                BufBuilder profileBufBuilder(1024);

                // NOTE: It's kind of weird that we lock the op's namespace, but have to for now
                // since we're sometimes inside the lock already
                const string dbname(nsToDatabase(currentOp.getNS()));
                scoped_ptr<Lock::DBLock> lk;

                // todo: this can be slow, perhaps can re-work
                if ( !txn->lockState()->isDbLockedForMode( dbname, MODE_IX ) ) {
                    lk.reset( new Lock::DBLock( txn->lockState(),
                                                dbname,
                                                tryAgain ? MODE_X : MODE_IX) );
                }
                Database* db = dbHolder().get(txn, dbname);
                if (db != NULL) {
                    Lock::CollectionLock clk(txn->lockState(), db->getProfilingNS(), MODE_X);
                    Client::Context cx(txn, currentOp.getNS(), false);
                    if ( !_profile(txn, c, cx.db(), currentOp, profileBufBuilder ) && lk.get() ) {
                        if ( tryAgain ) {
                            // we couldn't profile, but that's ok, we should have logged already
                            break;
                        }
                        // we took an IX lock, so now we try again with an X lock
                        tryAgain = true;
                        continue;
                    }
                }
                else {
                    mongo::log() << "note: not profiling because db went away - "
                                 << "probably a close on: " << currentOp.getNS();
                }
                return;
            }
            catch (const AssertionException& assertionEx) {
                warning() << "Caught Assertion while trying to profile " << opToString(op)
                          << " against " << currentOp.getNS()
                          << ": " << assertionEx.toString() << endl;
                return;
            }
        }
    }

    Collection* getOrCreateProfileCollection(OperationContext* txn,
                                             Database *db,
                                             bool force,
                                             string* errmsg ) {
        fassert(16372, db);
        const char* profileName = db->getProfilingNS();
        Collection* collection = db->getCollection( profileName );

        if ( collection ) {
            if ( !collection->isCapped() ) {
                string myerrmsg = str::stream() << profileName << " exists but isn't capped";
                log() << myerrmsg << endl;
                if ( errmsg )
                    *errmsg = myerrmsg;
                return NULL;
            }
            return collection;
        }

        // does not exist!

        if ( force == false && serverGlobalParams.defaultProfile == false ) {
            // we don't want it, so why are we here?
            static time_t last = time(0) - 10;  // warn the first time
            if( time(0) > last+10 ) {
                log() << "profile: warning ns " << profileName << " does not exist" << endl;
                last = time(0);
            }
            return NULL;
        }

        if ( !txn->lockState()->isDbLockedForMode( db->name(), MODE_X ) ) {
            // can't create here
            return NULL;
        }

        // system.profile namespace doesn't exist; create it
        log() << "creating profile collection: " << profileName << endl;

        CollectionOptions collectionOptions;
        collectionOptions.capped = true;
        collectionOptions.cappedSize = 1024 * 1024;

        WriteUnitOfWork wunit(txn);
        collection = db->createCollection( txn, profileName, collectionOptions );
        invariant( collection );
        wunit.commit();

        return collection;
    }

} // namespace mongo
