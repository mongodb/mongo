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

#include "mongo/pch.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_set.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/goodies.h"

namespace {
    const size_t MAX_PROFILE_DOC_SIZE_BYTES = 100*1024;
}

namespace mongo {

namespace {
    void _appendUserInfo(const CurOp& c,
                         BSONObjBuilder& builder,
                         AuthorizationSession* authSession) {
        UserNameIterator nameIter = authSession->getAuthenticatedUserNames();

        UserName bestUser;
        if (nameIter.more())
            bestUser = *nameIter;

        StringData opdb( nsToDatabaseSubstring( c.getNS() ) );

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

    static void _profile(OperationContext* txn,
                         const Client& c,
                         Database* db,
                         CurOp& currentOp,
                         BufBuilder& profileBufBuilder) {
        dassert( db );

        // build object
        BSONObjBuilder b(profileBufBuilder);

        const bool isQueryObjTooBig = !currentOp.debug().append(currentOp, b,
                MAX_PROFILE_DOC_SIZE_BYTES);

        b.appendDate("ts", jsTime());
        b.append("client", c.clientAddress());

        AuthorizationSession * authSession = c.getAuthorizationSession();
        _appendUserInfo(currentOp, b, authSession);

        BSONObj p = b.done();

        if (static_cast<size_t>(p.objsize()) > MAX_PROFILE_DOC_SIZE_BYTES || isQueryObjTooBig) {
            string small = p.toString(/*isArray*/false, /*full*/false);

            warning() << "can't add full line to system.profile: " << small << endl;

            // rebuild with limited info
            BSONObjBuilder b(profileBufBuilder);
            b.appendDate("ts", jsTime());
            b.append("client", c.clientAddress() );
            _appendUserInfo(currentOp, b, authSession);

            b.append("err", "profile line too large (max is 100KB)");

            // should be much smaller but if not don't break anything
            if (small.size() < MAX_PROFILE_DOC_SIZE_BYTES){
                b.append("abbreviated", small);
            }

            p = b.done();
        }

        // write: not replicated
        // get or create the profiling collection
        Collection* profileCollection = getOrCreateProfileCollection(txn, db);
        if ( profileCollection ) {
            profileCollection->insertDocument( txn, p, false );
        }
    }

    void profile(OperationContext* txn, const Client& c, int op, CurOp& currentOp) {
        // initialize with 1kb to start, to avoid realloc later
        // doing this outside the dblock to improve performance
        BufBuilder profileBufBuilder(1024);

        try {
            // NOTE: It's kind of weird that we lock the op's namespace, but have to for now since
            // we're sometimes inside the lock already
            Lock::DBWrite lk(txn->lockState(), currentOp.getNS() );
            if (dbHolder().get(txn, nsToDatabase(currentOp.getNS())) != NULL) {

                Client::Context cx(currentOp.getNS(), false);
                _profile(txn, c, cx.db(), currentOp, profileBufBuilder);
            }
            else {
                mongo::log() << "note: not profiling because db went away - probably a close on: "
                             << currentOp.getNS() << endl;
            }
        }
        catch (const AssertionException& assertionEx) {
            warning() << "Caught Assertion while trying to profile " << opToString(op)
                      << " against " << currentOp.getNS()
                      << ": " << assertionEx.toString() << endl;
        }
    }

    Collection* getOrCreateProfileCollection(OperationContext* txn,
                                             Database *db,
                                             bool force,
                                             string* errmsg ) {
        fassert(16372, db);
        const char* profileName = db->getProfilingNS();
        Collection* collection = db->getCollection( txn, profileName );

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

        // system.profile namespace doesn't exist; create it
        log() << "creating profile collection: " << profileName << endl;

        CollectionOptions collectionOptions;
        collectionOptions.capped = true;
        collectionOptions.cappedSize = 1024 * 1024;

        collection = db->createCollection( txn, profileName, collectionOptions );
        invariant( collection );
        return collection;
    }

} // namespace mongo
