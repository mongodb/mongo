// index_details.h

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

#pragma once

#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

/* Details about a particular index. There is one of these effectively for each object in
   system.namespaces (although this also includes the head pointer, which is not in that
   collection).

   This is an internal part of the catalog.  Nothing outside of the catalog should use this.

   ** MemoryMapped in NamespaceDetails ** (i.e., this is on disk data)
 */
#pragma pack(1)
struct IndexDetails {
    /**
     * btree head disk location
     */
    DiskLoc head;

    /* Location of index info object. Format:

         { name:"nameofindex", ns:"parentnsname", key: {keypattobject}
           [, unique: <bool>, background: <bool>, v:<version>]
         }

       This object is in the system.indexes collection.  Note that since we
       have a pointer to the object here, the object in system.indexes MUST NEVER MOVE.
    */
    DiskLoc info;

    /**
     * makes head and info invalid
    */
    void _reset();
};
#pragma pack()

}  // namespace mongo
