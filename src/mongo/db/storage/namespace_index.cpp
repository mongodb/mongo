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
 */

#include "mongo/db/storage/namespace_index.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/db/namespace_details.h"


namespace mongo {

    unsigned lenForNewNsFiles = 16 * 1024 * 1024;

    NamespaceDetails* NamespaceIndex::details(const StringData& ns) {
        Namespace n(ns);
        return details(n);
    }

    NamespaceDetails* NamespaceIndex::details(const Namespace& ns) {
        if ( !ht )
            return 0;
        NamespaceDetails *d = ht->get(ns);
        if ( d && d->isCapped() )
            d->cappedCheckMigrate();
        return d;
    }

    void NamespaceIndex::add_ns(const StringData& ns, DiskLoc& loc, bool capped) {
        NamespaceDetails details( loc, capped );
        add_ns( ns, &details );
    }

    void NamespaceIndex::add_ns( const StringData& ns, const NamespaceDetails* details ) {
        Namespace n(ns);
        add_ns( n, details );
    }

    void NamespaceIndex::add_ns( const Namespace& ns, const NamespaceDetails* details ) {
        Lock::assertWriteLocked(ns.toString());
        init();
        uassert( 10081 , "too many namespaces/collections", ht->put(ns, *details));
    }

    void NamespaceIndex::kill_ns(const char *ns) {
        Lock::assertWriteLocked(ns);
        if ( !ht )
            return;
        Namespace n(ns);
        ht->kill(n);

        for( int i = 0; i<=1; i++ ) {
            try {
                Namespace extra(n.extraName(i));
                ht->kill(extra);
            }
            catch(DBException&) {
                dlog(3) << "caught exception in kill_ns" << endl;
            }
        }
    }

    bool NamespaceIndex::exists() const {
        return !boost::filesystem::exists(path());
    }

    boost::filesystem::path NamespaceIndex::path() const {
        boost::filesystem::path ret( dir_ );
        if ( directoryperdb )
            ret /= database_;
        ret /= ( database_ + ".ns" );
        return ret;
    }

    static void namespaceGetNamespacesCallback( const Namespace& k , NamespaceDetails& v , void * extra ) {
        list<string> * l = (list<string>*)extra;
        if ( ! k.hasDollarSign() )
            l->push_back( (string)k );
    }

    void NamespaceIndex::getNamespaces( list<string>& tofill , bool onlyCollections ) const {
        verify( onlyCollections ); // TODO: need to implement this
        //                                  need boost::bind or something to make this less ugly

        if ( ht )
            ht->iterAll( namespaceGetNamespacesCallback , (void*)&tofill );
    }

    void NamespaceIndex::maybeMkdir() const {
        if ( !directoryperdb )
            return;
        boost::filesystem::path dir( dir_ );
        dir /= database_;
        if ( !boost::filesystem::exists( dir ) )
            MONGO_ASSERT_ON_EXCEPTION_WITH_MSG( boost::filesystem::create_directory( dir ), "create dir for db " );
    }

    NOINLINE_DECL void NamespaceIndex::_init() {
        verify( !ht );

        Lock::assertWriteLocked(database_);

        /* if someone manually deleted the datafiles for a database,
           we need to be sure to clear any cached info for the database in
           local.*.
        */
        /*
        if ( "local" != database_ ) {
            DBInfo i(database_.c_str());
            i.dbDropped();
        }
        */

        unsigned long long len = 0;
        boost::filesystem::path nsPath = path();
        string pathString = nsPath.string();
        void *p = 0;
        if( boost::filesystem::exists(nsPath) ) {
            if( f.open(pathString, true) ) {
                len = f.length();
                if ( len % (1024*1024) != 0 ) {
                    log() << "bad .ns file: " << pathString << endl;
                    uassert( 10079 ,  "bad .ns file length, cannot open database", len % (1024*1024) == 0 );
                }
                p = f.getView();
            }
        }
        else {
            // use lenForNewNsFiles, we are making a new database
            massert( 10343, "bad lenForNewNsFiles", lenForNewNsFiles >= 1024*1024 );
            maybeMkdir();
            unsigned long long l = lenForNewNsFiles;
            if( f.create(pathString, l, true) ) {
                getDur().createdFile(pathString, l); // always a new file
                len = l;
                verify( len == lenForNewNsFiles );
                p = f.getView();
            }
        }

        if ( p == 0 ) {
            /** TODO: this shouldn't terminate? */
            log() << "error couldn't open file " << pathString << " terminating" << endl;
            dbexit( EXIT_FS );
        }


        verify( len <= 0x7fffffff );
        ht = new HashTable<Namespace,NamespaceDetails>(p, (int) len, "namespace index");
    }


}

