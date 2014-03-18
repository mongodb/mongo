// database.h

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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/db/storage/extent.h"

namespace mongo {

    /* Record is a record in a datafile.  DeletedRecord is similar but for deleted space.

    *11:03:20 AM) dm10gen: regarding extentOfs...
    (11:03:42 AM) dm10gen: an extent is a continugous disk area, which contains many Records and DeleteRecords
    (11:03:56 AM) dm10gen: a DiskLoc has two pieces, the fileno and ofs.  (64 bit total)
    (11:04:16 AM) dm10gen: to keep the headesr small, instead of storing a 64 bit ptr to the full extent address, we keep just the offset
    (11:04:29 AM) dm10gen: we can do this as we know the record's address, and it has the same fileNo
    (11:04:33 AM) dm10gen: see class DiskLoc for more info
    (11:04:43 AM) dm10gen: so that is how Record::myExtent() works
    (11:04:53 AM) dm10gen: on an alloc(), when we build a new Record, we must populate its extentOfs then
    */
#pragma pack(1)
    class Record {
    public:
        enum HeaderSizeValue { HeaderSize = 16 };

        int lengthWithHeaders() const {  _accessing(); return _lengthWithHeaders; }
        int& lengthWithHeaders() {  _accessing(); return _lengthWithHeaders; }

        int extentOfs() const { _accessing(); return _extentOfs; }
        int& extentOfs() { _accessing(); return _extentOfs; }

        int nextOfs() const { _accessing(); return _nextOfs; }
        int& nextOfs() { _accessing(); return _nextOfs; }

        int prevOfs() const {  _accessing(); return _prevOfs; }
        int& prevOfs() {  _accessing(); return _prevOfs; }

        const char * data() const { _accessing(); return _data; }
        char * data() { _accessing(); return _data; }

        const char * dataNoThrowing() const { return _data; }
        char * dataNoThrowing() { return _data; }

        int netLength() const { _accessing(); return _netLength(); }

        /* use this when a record is deleted. basically a union with next/prev fields */
        DeletedRecord& asDeleted() { return *((DeletedRecord*) this); }

        // TODO(ERH): remove
        Extent* myExtent(const DiskLoc& myLoc) { return DiskLoc(myLoc.a(), extentOfs() ).ext(); }

        struct NP {
            int nextOfs;
            int prevOfs;
        };
        NP* np() { return (NP*) &_nextOfs; }

        // ---------------------
        // memory cache
        // ---------------------

        /**
         * touches the data so that is in physical memory
         * @param entireRecrd if false, only the header and first byte is touched
         *                    if true, the entire record is touched
         * */
        void touch( bool entireRecrd = false ) const;

        /**
         * @return if this record is likely in physical memory
         *         its not guaranteed because its possible it gets swapped out in a very unlucky windows
         */
        bool likelyInPhysicalMemory() const ;

        /**
         * tell the cache this Record was accessed
         * @return this, for simple chaining
         */
        Record* accessed();

        static bool likelyInPhysicalMemory( const char* data );

        /**
         * this adds stats about page fault exceptions currently
         * specically how many times we call _accessing where the record is not in memory
         * and how many times we throw a PageFaultException
         */
        static void appendStats( BSONObjBuilder& b );

        static void appendWorkingSetInfo( BSONObjBuilder& b );
    private:

        int _netLength() const { return _lengthWithHeaders - HeaderSize; }

        /**
         * call this when accessing a field which could hit disk
         */
        void _accessing() const;

        int _lengthWithHeaders;
        int _extentOfs;
        int _nextOfs;
        int _prevOfs;

        /** be careful when referencing this that your write intent was correct */
        char _data[4];

    public:
        static bool MemoryTrackingEnabled;

    };
#pragma pack()

    // TODO: this probably moves to record_store.h
    class DeletedRecord {
    public:

        int lengthWithHeaders() const { _accessing(); return _lengthWithHeaders; }
        int& lengthWithHeaders() { _accessing(); return _lengthWithHeaders; }

        int extentOfs() const { _accessing(); return _extentOfs; }
        int& extentOfs() { _accessing(); return _extentOfs; }

        // TODO: we need to not const_cast here but problem is DiskLoc::writing
        DiskLoc& nextDeleted() const { _accessing(); return const_cast<DiskLoc&>(_nextDeleted); }

    private:

        void _accessing() const;

        int _lengthWithHeaders;
        int _extentOfs;
        DiskLoc _nextDeleted;
    };

    BOOST_STATIC_ASSERT( 16 == sizeof(DeletedRecord) );

    struct RecordStats {
        void record( BSONObjBuilder& b );

        AtomicInt64 accessesNotInMemory;
        AtomicInt64 pageFaultExceptionsThrown;
    };

}
