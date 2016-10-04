// extent.h

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

#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "mongo/db/storage/mmap_v1/catalog/namespace.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

/* extents are datafile regions where all the records within the region
   belong to the same namespace.

(11:12:35 AM) dm10gen: when the extent is allocated, all its empty space is stuck into one big
    DeletedRecord
(11:12:55 AM) dm10gen: and that is placed on the free list
*/
#pragma pack(1)
struct Extent {
    enum { extentSignature = 0x41424344 };
    unsigned magic;
    DiskLoc myLoc;

    /* next/prev extent for this namespace */
    DiskLoc xnext;
    DiskLoc xprev;

    /* which namespace this extent is for.  this is just for troubleshooting really
       and won't even be correct if the collection were renamed!
    */
    Namespace nsDiagnostic;

    int length; /* size of the extent, including these fields */
    DiskLoc firstRecord;
    DiskLoc lastRecord;
    char _extentData[4];

    // -----

    bool validates(const DiskLoc diskLoc, std::vector<std::string>* errors = NULL) const;

    BSONObj dump() const;

    void dump(std::iostream& s) const;

    bool isOk() const {
        return magic == extentSignature;
    }
    void assertOk() const {
        verify(isOk());
    }

    static int HeaderSize() {
        return sizeof(Extent) - 4;
    }
};
#pragma pack()
}
