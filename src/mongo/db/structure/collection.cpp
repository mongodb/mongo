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

namespace mongo {

    CollectionTemp::CollectionTemp( const StringData& fullNS,
                                    NamespaceDetails* details,
                                    Database* database ) {
        _ns = fullNS.toString();
        _details = details;
        _database = database;
    }

}
