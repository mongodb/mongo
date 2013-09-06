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

#include "mongo/db/diskloc.h"
#include "mongo/db/storage/namespace.h"

namespace mongo {

    /* extents are datafile regions where all the records within the region
       belong to the same namespace.

    (11:12:35 AM) dm10gen: when the extent is allocated, all its empty space is stuck into one big DeletedRecord
    (11:12:55 AM) dm10gen: and that is placed on the free list
    */
#pragma pack(1)
    class Extent {
    public:
        enum { extentSignature = 0x41424344 };
        unsigned magic;
        DiskLoc myLoc;
        DiskLoc xnext, xprev; /* next/prev extent for this namespace */

        /* which namespace this extent is for.  this is just for troubleshooting really
           and won't even be correct if the collection were renamed!
        */
        Namespace nsDiagnostic;

        int length;   /* size of the extent, including these fields */
        DiskLoc firstRecord;
        DiskLoc lastRecord;
        char _extentData[4];

        static int HeaderSize() { return sizeof(Extent)-4; }

        bool validates(const DiskLoc diskLoc, BSONArrayBuilder* errors = NULL);

        BSONObj dump();

        void dump(iostream& s);

        /* assumes already zeroed -- insufficient for block 'reuse' perhaps
        Returns a DeletedRecord location which is the data in the extent ready for us.
        Caller will need to add that to the freelist structure in namespacedetail.
        */
        DiskLoc init(const char *nsname, int _length, int _fileNo, int _offset, bool capped);

        /* like init(), but for a reuse case */
        DiskLoc reuse(const char *nsname, bool newUseIsAsCapped);

        bool isOk() const { return magic == extentSignature; }
        void assertOk() const { verify(isOk()); }

        Record* getRecord(DiskLoc dl) {
            verify( !dl.isNull() );
            verify( dl.sameFile(myLoc) );
            int x = dl.getOfs() - myLoc.getOfs();
            verify( x > 0 );
            return (Record *) (((char *) this) + x);
        }

        Extent* getNextExtent();
        Extent* getPrevExtent();

        static int maxSize();
        static int minSize() { return 0x1000; }
        /**
         * @param len lengt of record we need
         * @param lastRecord size of last extent which is a factor in next extent size
         */
        static int followupSize(int len, int lastExtentLen);

        /** get a suggested size for the first extent in a namespace
         *  @param len length of record we need to insert
         */
        static int initialSize(int len);

        struct FL {
            DiskLoc firstRecord;
            DiskLoc lastRecord;
        };
        /** often we want to update just the firstRecord and lastRecord fields.
            this helper is for that -- for use with getDur().writing() method
        */
        FL* fl() { return (FL*) &firstRecord; }

        /** caller must declare write intent first */
        void markEmpty();
    private:
        DiskLoc _reuse(const char *nsname, bool newUseIsAsCapped); // recycle an extent and reuse it for a different ns
    };

#pragma pack()

}
