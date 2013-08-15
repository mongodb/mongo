// collection.cpp

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

#include "mongo/db/structure/collection.h"

#include "mongo/db/database.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/structure/collection_iterator.h"

namespace mongo {

    CollectionTemp::CollectionTemp( const StringData& fullNS,
                                    NamespaceDetails* details,
                                    Database* database ) {
        _ns = fullNS.toString();
        _details = details;
        _database = database;
    }

    CollectionIterator* CollectionTemp::getIterator( const DiskLoc& start, bool tailable,
                                                     const CollectionScanParams::Direction& dir) const {
        if ( _details->isCapped() )
            return new CappedIterator( _ns, start, tailable, dir );
        return new FlatIterator( this, start, dir );
    }


    ExtentManager* CollectionTemp::getExtentManager() {
        return &_database->getExtentManager();
    }

    const ExtentManager* CollectionTemp::getExtentManager() const {
        return &_database->getExtentManager();
    }

}
