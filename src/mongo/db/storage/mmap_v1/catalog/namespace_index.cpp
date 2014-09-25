// namespace_index.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndexing

#include "mongo/platform/basic.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_index.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/util/exit.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"

namespace mongo {

    NamespaceDetails* NamespaceIndex::details(const StringData& ns) {
        Namespace n(ns);
        return details(n);
    }

    NamespaceDetails* NamespaceIndex::details(const Namespace& ns) {
        if ( !_ht.get() )
            return 0;
        return _ht->get(ns);
    }

    void NamespaceIndex::add_ns( OperationContext* txn,
                                 const StringData& ns, const DiskLoc& loc, bool capped) {
        NamespaceDetails details( loc, capped );
        add_ns( txn, ns, &details );
    }

    void NamespaceIndex::add_ns( OperationContext* txn,
                                 const StringData& ns, const NamespaceDetails* details ) {
        Namespace n(ns);
        add_ns( txn, n, details );
    }

    void NamespaceIndex::add_ns( OperationContext* txn,
                                 const Namespace& ns, const NamespaceDetails* details ) {
        string nsString = ns.toString();
        txn->lockState()->assertWriteLocked( nsString );
        massert( 17315, "no . in ns", nsString.find( '.' ) != string::npos );
        init( txn );
        uassert( 10081, "too many namespaces/collections", _ht->put(txn, ns, *details));
    }

    void NamespaceIndex::kill_ns( OperationContext* txn, const StringData& ns) {
        txn->lockState()->assertWriteLocked(ns);
        if ( !_ht.get() )
            return;
        Namespace n(ns);
        _ht->kill(txn, n);

        if (ns.size() <= Namespace::MaxNsColletionLen) {
            // Larger namespace names don't have room for $extras so they can't exist. The code
            // below would cause an "$extra: ns too large" error and stacktrace to be printed to the
            // log even though everything is fine.
            for( int i = 0; i<=1; i++ ) {
                try {
                    Namespace extra(n.extraName(i));
                    _ht->kill(txn, extra);
                }
                catch(DBException&) {
                    LOG(3) << "caught exception in kill_ns" << endl;
                }
            }
        }
    }

    bool NamespaceIndex::pathExists() const {
        return boost::filesystem::exists(path());
    }

    boost::filesystem::path NamespaceIndex::path() const {
        boost::filesystem::path ret( _dir );
        if (storageGlobalParams.directoryperdb)
            ret /= _database;
        ret /= ( _database + ".ns" );
        return ret;
    }

    static void namespaceGetNamespacesCallback( const Namespace& k , NamespaceDetails& v , list<string>* l ) {
        if ( ! k.hasDollarSign() || k == "local.oplog.$main" ) {
            // we call out local.oplog.$main specifically as its the only "normal"
            // collection that has a $, so we make sure it gets added
            l->push_back( k.toString() );
        }
    }

    void NamespaceIndex::getCollectionNamespaces( list<string>* tofill ) const {
        if ( _ht.get() )
            _ht->iterAll( stdx::bind( namespaceGetNamespacesCallback,
                                      stdx::placeholders::_1, stdx::placeholders::_2, tofill) );
    }

    void NamespaceIndex::maybeMkdir() const {
        if (!storageGlobalParams.directoryperdb)
            return;
        boost::filesystem::path dir( _dir );
        dir /= _database;
        if ( !boost::filesystem::exists( dir ) )
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( boost::filesystem::create_directory( dir ), "create dir for db " );
    }

    NOINLINE_DECL void NamespaceIndex::_init( OperationContext* txn ) {
        verify( !_ht.get() );

        txn->lockState()->assertWriteLocked(_database);

        /* if someone manually deleted the datafiles for a database,
           we need to be sure to clear any cached info for the database in
           local.*.
        */
        /*
        if ( "local" != _database ) {
            DBInfo i(_database.c_str());
            i.dbDropped();
        }
        */

        unsigned long long len = 0;
        boost::filesystem::path nsPath = path();
        string pathString = nsPath.string();
        void *p = 0;
        if ( boost::filesystem::exists(nsPath) ) {
            if( _f.open(pathString, true) ) {
                len = _f.length();
                if ( len % (1024*1024) != 0 ) {
                    log() << "bad .ns file: " << pathString << endl;
                    uassert( 10079 ,  "bad .ns file length, cannot open database", len % (1024*1024) == 0 );
                }
                p = _f.getView();
            }
        }
        else {
            // use storageGlobalParams.lenForNewNsFiles, we are making a new database
            massert(10343, "bad storageGlobalParams.lenForNewNsFiles",
                    storageGlobalParams.lenForNewNsFiles >= 1024*1024);
            maybeMkdir();
            unsigned long long l = storageGlobalParams.lenForNewNsFiles;

            {
                // Due to SERVER-15369 we need to explicitly write zero-bytes to the NS file.
                const unsigned long long kBlockSize = 1024*1024;
                invariant(l % kBlockSize == 0); // ns files can only be multiples of 1MB
                const std::vector<char> zeros(kBlockSize, 0);

                File file;
                file.open(pathString.c_str());
                massert(18825, str::stream() << "couldn't create file " << pathString, file.is_open());
                for (fileofs ofs = 0; ofs < l; ofs += kBlockSize ) {
                    file.write(ofs, &zeros[0], kBlockSize);
                }
                file.fsync();
                massert(18826, str::stream() << "failure writing file " << pathString, !file.bad() );
            }

            if ( _f.create(pathString, l, true) ) {
                // The writes done in this function must not be rolled back. If the containing
                // UnitOfWork rolls back it should roll back to the state *after* these writes. This
                // will leave the file empty, but available for future use. That is why we go
                // directly to the global dur dirty list rather than going through the
                // OperationContext.
                getDur().createdFile(pathString, l); // always a new file
                len = l;
                verify(len == storageGlobalParams.lenForNewNsFiles);
                p = _f.getView();

                if ( p ) {
                    // we do this so the durability system isn't mad at us for
                    // only initiating file and not doing a write
                    // grep for 17388
                    getDur().writingPtr( p, 5 ); // throw away
                }
            }
        }

        if ( p == 0 ) {
            /** TODO: this shouldn't terminate? */
            log() << "error couldn't open file " << pathString << " terminating" << endl;
            dbexit( EXIT_FS );
        }


        verify( len <= 0x7fffffff );
        _ht.reset(new NamespaceHashTable(p, (int) len, "namespace index"));
    }


}

