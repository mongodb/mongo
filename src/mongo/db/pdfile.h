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

/* pdfile.h

   Files:
     database.ns - namespace index
     database.1  - data files
     database.2
     ...
*/

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/database.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/memconcept.h"
#include "mongo/db/storage/data_file.h"
#include "mongo/db/storage/durable_mapped_file.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/namespace_details-inl.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pdfile_version.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/log.h"
#include "mongo/util/mmap.h"

namespace mongo {

    class DataFileHeader;
    class Extent;
    class OpDebug;
    class Record;
    struct SortPhaseOne;

    void dropDatabase(const std::string& db);
    bool repairDatabase(string db, string &errmsg, bool preserveClonedFilesOnFailure = false, bool backupOriginalFiles = false);

    bool userCreateNS(const char *ns, BSONObj j, string& err, bool logForReplication, bool *deferIdIndex = 0);

    bool isValidNS( const StringData& ns );

    /*---------------------------------------------------------------------*/

    template< class V >
    inline
    const BtreeBucket<V> * DiskLoc::btree() const {
        verify( _a != -1 );
        Record *r = rec();
        memconcept::is(r, memconcept::concept::btreebucket, "", 8192);
        return (const BtreeBucket<V> *) r->data();
    }



    boost::intmax_t dbSize( const char *database );

    inline NamespaceIndex* nsindex(const StringData& ns) {
        Database *database = cc().database();
        verify( database );
        memconcept::is(database, memconcept::concept::database, ns, sizeof(Database));
        DEV {
            StringData dbname = nsToDatabaseSubstring( ns );
            if ( database->name() != dbname ) {
                out() << "ERROR: attempt to write to wrong database\n";
                out() << " ns:" << ns << '\n';
                out() << " database->name:" << database->name() << endl;
                verify( database->name() == dbname );
            }
        }
        return &database->namespaceIndex();
    }

    inline NamespaceDetails* nsdetails(const StringData& ns) {
        // if this faults, did you set the current db first?  (Client::Context + dblock)
        NamespaceDetails *d = nsindex(ns)->details(ns);
        if( d ) {
            memconcept::is(d, memconcept::concept::nsdetails, ns, sizeof(NamespaceDetails));
        }
        return d;
    }

    BOOST_STATIC_ASSERT( 16 == sizeof(DeletedRecord) );

    inline BSONObj BSONObj::make(const Record* r ) {
        return BSONObj( r->data() );
    }

    DiskLoc allocateSpaceForANewRecord(const char* ns,
                                       NamespaceDetails* d,
                                       int32_t lenWHdr,
                                       bool god);

    void addRecordToRecListInExtent(Record* r, DiskLoc loc);


} // namespace mongo
