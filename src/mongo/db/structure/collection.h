// collection.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/structure/collection_info_cache.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    class Database;
    class ExtentManager;
    class NamespaceDetails;

    class CollectionIterator;
    class FlatIterator;
    class CappedIterator;

    /**
     * this is NOT safe through a yield right now
     * not sure if it will be, or what yet
     */
    class Collection {
    public:
        Collection( const StringData& fullNS,
                        NamespaceDetails* details,
                        Database* database );

        ~Collection();

        bool ok() const { return _magic == 1357924; }

        NamespaceDetails* details() { return _details; } // TODO: remove
        const NamespaceDetails* details() const { return _details; }

        CollectionInfoCache* infoCache() { return &_infoCache; }
        const CollectionInfoCache* infoCache() const { return &_infoCache; }

        const NamespaceString& ns() const { return _ns; }

        BSONObj docFor( const DiskLoc& loc );

        CollectionIterator* getIterator( const DiskLoc& start, bool tailable,
                                         const CollectionScanParams::Direction& dir) const;

        void deleteDocument( const DiskLoc& loc,
                             bool cappedOK = false,
                             bool noWarn = false,
                             BSONObj* deletedId = 0 );

        // this is temporary, moving up from DB for now
        // this will add a new extent the collection
        // the new extent will be returned
        // it will have been added to the linked list already
        Extent* increaseStorageSize( int size, bool enforceQuota );

        //
        // Stats
        //

        uint64_t numRecords() const;

    private:

        // @return 0 for inf., otherwise a number of files
        int largestFileNumberInQuota() const;

        ExtentManager* getExtentManager();
        const ExtentManager* getExtentManager() const;

        int _magic;

        NamespaceString _ns;
        NamespaceDetails* _details;
        Database* _database;
        RecordStore _recordStore;
        CollectionInfoCache _infoCache;


        friend class Database;
        friend class FlatIterator;
        friend class CappedIterator;
    };

}
