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

/* @file diskloc.h

   Storage subsystem management.
   Lays out our datafiles on disk, manages disk space.
*/

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    class Record;
    class DeletedRecord;
    class Extent;
    class DataFile;
    class DiskLoc;

    template< class Version > class BtreeBucket;

#pragma pack(1)
    /** represents a disk location/offset on disk in a database.  64 bits.
        it is assumed these will be passed around by value a lot so don't do anything to make them large
        (such as adding a virtual function)
     */
    class DiskLoc {
        int _a;     // this will be volume, file #, etc. but is a logical value could be anything depending on storage engine
        int ofs;

    public:

        enum SentinelValues {
            /* note NullOfs is different. todo clean up.  see refs to NullOfs in code - use is valid but outside DiskLoc context so confusing as-is. */
            NullOfs = -1,

            // Caps the number of files that may be allocated in a database, allowing about 32TB of
            // data per db.  Note that the DiskLoc and DiskLoc56Bit types supports more files than
            // this value, as does the data storage format.
            MaxFiles=16000
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
        void assertOk() const { verify(!isNull()); }
        void setInvalid() {
            _a = -2;
            ofs = 0;
        }
        bool isValid() const { return _a != -2; }

        string toString() const {
            if ( isNull() )
                return "null";
            stringstream ss;
            ss << _a << ':' << hex << ofs;
            return ss.str();
        }

        BSONObj toBSONObj() const { return BSON( "file" << _a << "offset" << ofs );  }

        int a() const { return _a; }

        int& GETOFS()      { return ofs; }
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
         * Hash value for this disk location.  The hash implementation may be modified, and its
         * behavior may differ across platforms.  Hash values should not be persisted.
         */
        struct Hasher {
            size_t operator()( DiskLoc loc ) const;
        };

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

        /*DataFile& pdf() const;*/

        /// members for Sorter
        struct SorterDeserializeSettings {}; // unused
        void serializeForSorter(BufBuilder& buf) const { buf.appendStruct(*this); }
        static DiskLoc deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
            return buf.read<DiskLoc>();
        }
        int memUsageForSorter() const { return sizeof(DiskLoc); }
        DiskLoc getOwned() const { return *this; }
    };
#pragma pack()

    inline size_t DiskLoc::Hasher::operator()( DiskLoc loc ) const {
        // Older tr1 implementations do not support hashing 64 bit integers.  This implementation
        // delegates to hashing 32 bit integers.
        return
            unordered_set<uint32_t>::hasher()( loc.a() ) ^
            unordered_set<uint32_t>::hasher()( loc.getOfs() );
    }

    inline std::ostream& operator<<( std::ostream &stream, const DiskLoc &loc ) {
        return stream << loc.toString();
    }

    // Minimum allowed DiskLoc.  No Record may begin at this location because file and extent
    // headers must precede Records in a file.
    const DiskLoc minDiskLoc(0, 0);

    // Maximum allowed DiskLoc.  Note that only three bytes are used to represent the file number
    // for consistency with the v1 index DiskLoc storage format, which uses only 7 bytes total.
    // No Record may begin at this location because the minimum size of a Record is larger than one
    // byte.
    const DiskLoc maxDiskLoc(0x00ffffff, 0x7fffffff);

} // namespace mongo
