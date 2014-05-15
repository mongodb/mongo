// pdfile.cpp

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

/*
todo:
_ table scans must be sequential, not next/prev pointers
_ coalesce deleted
_ disallow system* manipulations from the database.
*/

#include "mongo/pch.h"

#include "mongo/db/pdfile.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <boost/optional/optional.hpp>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/pdfile_private.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/db.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/extsort.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/instance.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/file.h"
#include "mongo/util/file_allocator.h"
#include "mongo/util/mmap.h"
#include "mongo/util/processinfo.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/operation_context_impl.h"

namespace mongo {

    /* ----------------------------------------- */
    string pidfilepath;

    DatabaseHolder _dbHolder;

    DatabaseHolder& dbHolderUnchecked() {
        return _dbHolder;
    }

    /*---------------------------------------------------------------------*/

    /** { ..., capped: true, size: ..., max: ... }
     * @param createDefaultIndexes - if false, defers id (and other) index creation.
     * @return true if successful
    */
    Status userCreateNS( OperationContext* txn,
                         Database* db,
                         const StringData& ns,
                         BSONObj options,
                         bool logForReplication,
                         bool createDefaultIndexes ) {

        invariant( db );

        LOG(1) << "create collection " << ns << ' ' << options;

        if ( !NamespaceString::validCollectionComponent(ns) )
            return Status( ErrorCodes::InvalidNamespace,
                           str::stream() << "invalid ns: " << ns );

        Collection* collection = db->getCollection( ns );

        if ( collection )
            return Status( ErrorCodes::NamespaceExists,
                           "collection already exists" );

        CollectionOptions collectionOptions;
        Status status = collectionOptions.parse( options );
        if ( !status.isOK() )
            return status;

        invariant( db->createCollection( txn, ns, collectionOptions, true, createDefaultIndexes ) );

        if ( logForReplication ) {
            if ( options.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << nsToCollectionSubstring( ns );
                b.appendElements( options );
                options = b.obj();
            }
            string logNs = nsToDatabase(ns) + ".$cmd";
            logOp(txn, "c", logNs.c_str(), options);
        }

        return Status::OK();
    }

    void dropAllDatabasesExceptLocal() {
        Lock::GlobalWrite lk;
        OperationContextImpl txn;

        vector<string> n;
        getDatabaseNames(n);
        if( n.size() == 0 ) return;
        log() << "dropAllDatabasesExceptLocal " << n.size() << endl;
        for( vector<string>::iterator i = n.begin(); i != n.end(); i++ ) {
            if( *i != "local" ) {
                Client::Context ctx(*i);
                dropDatabase(&txn, ctx.db());
            }
        }
    }

    void dropDatabase(OperationContext* txn, Database* db ) {
        invariant( db );

        string name = db->name(); // just to have safe
        LOG(1) << "dropDatabase " << name << endl;

        Lock::assertWriteLocked( name );

        BackgroundOperation::assertNoBgOpInProgForDb(name.c_str());

        audit::logDropDatabase( currentClient.get(), name );

        // Not sure we need this here, so removed.  If we do, we need to move it down
        // within other calls both (1) as they could be called from elsewhere and
        // (2) to keep the lock order right - groupcommitmutex must be locked before
        // mmmutex (if both are locked).
        //
        //  RWLockRecursive::Exclusive lk(MongoFile::mmmutex);

        txn->recoveryUnit()->syncDataAndTruncateJournal();

        Database::closeDatabase( name, db->path() );
        db = 0; // d is now deleted

        _deleteDataFiles( name );
    }

} // namespace mongo
