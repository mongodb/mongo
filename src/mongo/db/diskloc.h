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
*/

/* @file diskloc.h

   Storage subsystem management.
   Lays out our datafiles on disk, manages disk space.
*/

#pragma once

#include "jsobj.h"

namespace mongo {

    class Record;
    class DeletedRecord;
    class Extent;
    class MongoDataFile;
    class DiskLoc;

    template< class Version > class BtreeBucket;

#pragma pack(1)
    /** represents a disk location/offset on disk in a database.  64 bits.
        it is assumed these will be passed around by value a lot so don't do anything to make them large
        (such as adding a virtual function)
     */
    class DiskLoc {
        little<int> _a;     // this will be volume, file #, etsc. but is a logical value could be anything depending on storage engine
        little<int> ofs;

    public:

        enum SentinelValues {
            /* note NullOfs is different. todo clean up.  see refs to NullOfs in code - use is valid but outside DiskLoc context so confusing as-is. */
            NullOfs = -1,
            MaxFiles=16000 // thus a limit of about 32TB of data per db
        };

        DiskLoc(int a, int Ofs) : _a(a), ofs(Ofs) { }
        DiskLoc() { Null(); }
        DiskLoc(const DiskLoc& l) {
            _a=l._a;
            ofs=l.ofs;
        }

        bool questionable() const {
            return ofs < -1 ||
                   _a < -1 ||
                   _a > 524288;
        }

        bool isNull() const { return _a == -1; }
        void Null() {
            _a = -1;
            ofs = 0; /* note NullOfs is different. todo clean up.  see refs to NullOfs in code - use is valid but outside DiskLoc context so confusing as-is. */
        }
        void assertOk() { verify(!isNull()); }
        void setInvalid() {
            _a = -2;
            ofs = 0;
        }
        bool isValid() const { return _a != -2; }

        string toString() const {
            if ( isNull() )
                return "null";
            stringstream ss;
            ss << hex << _a << ':' << ofs;
            return ss.str();
        }

        BSONObj toBSONObj() const { return BSON( "file" << _a << "offset" << ofs );  }

        int a() const { return _a; }

        little<int>& GETOFS()      { return ofs; }
        int getOfs() const { return ofs; }
        void set(int a, int b) {
            _a=a;
            ofs=b;
        }

        void inc(int amt) {
            verify( !isNull() );
            ofs += amt;
        }

        bool sameFile(DiskLoc b) {
            return _a== b._a;
        }

        bool operator==(const DiskLoc& b) const {
            return _a==b._a&& ofs == b.ofs;
        }
        bool operator!=(const DiskLoc& b) const {
            return !(*this==b);
        }
        const DiskLoc& operator=(const DiskLoc& b) {
            _a=b._a;
            ofs = b.ofs;
            //verify(ofs!=0);
            return *this;
        }
        int compare(const DiskLoc& b) const {
            int x = _a - b._a;
            if ( x )
                return x;
            return ofs - b.ofs;
        }
        bool operator<(const DiskLoc& b) const {
            return compare(b) < 0;
        }

        /**
         * Marks this disk loc for writing
         * @returns a non const reference to this disk loc
         * This function explicitly signals we are writing and casts away const
         */
        DiskLoc& writing() const; // see dur.h

        /* Get the "thing" associated with this disk location.
           it is assumed the object is what you say it is -- you must assure that
           (think of this as an unchecked type cast)
           Note: set your Context first so that the database to which the diskloc applies is known.
        */
        BSONObj obj() const;
        Record* rec() const;
        DeletedRecord* drec() const;
        Extent* ext() const;

        template< class V >
        const BtreeBucket<V> * btree() const;

        // Explicitly signals we are writing and casts away const
        template< class V >
        BtreeBucket<V> * btreemod() const;

        /*MongoDataFile& pdf() const;*/
    };
#pragma pack()

    const DiskLoc minDiskLoc(0, 1);
    const DiskLoc maxDiskLoc(0x7fffffff, 0x7fffffff);

} // namespace mongo
