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
    class BtreeBucket;
    class MongoDataFile;

#pragma pack(1)
	/** represents a disk location/offset on disk in a database.  64 bits. 
		it is assumed these will be passed around by value a lot so don't do anything to make them large 
		(such as adding a virtual function)
	 */
    class DiskLoc {
        int fileNo;  // this will be volume, file #, etc. but is a logical value could be anything depending on storage engine
        int ofs;

    public:

        enum SentinelValues { 
		  MaxFiles=16000, // thus a limit of about 32TB of data per db
		  NullOfs = -1 
		};

	    DiskLoc(int a, int b) : fileNo(a), ofs(b) { }
        DiskLoc() { Null(); }
        DiskLoc(const DiskLoc& l) {
            fileNo=l.fileNo;
            ofs=l.ofs;
        }

        bool questionable() const {
            return ofs < -1 ||
                   fileNo < -1 ||
                   fileNo > 524288;
        }

        bool isNull() const { return fileNo == -1; }
        void Null() {
            fileNo = NullOfs;
            ofs = 0;
        }
        void assertOk() { assert(!isNull()); }
        void setInvalid() {
            fileNo = -2; 
            ofs = 0;
        }
        bool isValid() const {
            return fileNo != -2;
        }

        string toString() const {
            if ( isNull() )
                return "null";
            stringstream ss;
            ss << hex << fileNo << ':' << ofs;
            return ss.str();
        }

        BSONObj toBSONObj() const {
            return BSON( "file" << fileNo << "offset" << ofs );
        }

        int a() const      { return fileNo; }

        int& GETOFS()      { return ofs; }
        int getOfs() const { return ofs; }
        void set(int a, int b) {
            fileNo=a;
            ofs=b;
        }
        void setOfs(int _fileNo, int _ofs) {
            fileNo = _fileNo;
            ofs = _ofs;
        }

        void inc(int amt) {
            assert( !isNull() );
            ofs += amt;
        }

        bool sameFile(DiskLoc b) {
            return fileNo == b.fileNo;
        }

        bool operator==(const DiskLoc& b) const {
            return fileNo==b.fileNo && ofs == b.ofs;
        }
        bool operator!=(const DiskLoc& b) const {
            return !(*this==b);
        }
        const DiskLoc& operator=(const DiskLoc& b) {
            fileNo=b.fileNo;
            ofs = b.ofs;
            //assert(ofs!=0);
            return *this;
        }
        int compare(const DiskLoc& b) const {
            int x = fileNo - b.fileNo;
            if ( x )
                return x;
            return ofs - b.ofs;
        }
        bool operator<(const DiskLoc& b) const {
            return compare(b) < 0;
        }

        /* Get the "thing" associated with this disk location.
           it is assumed the object is what you say it is -- you must assure that
           (think of this as an unchecked type cast)
           Note: set your Context first so that the database to which the diskloc applies is known.
        */
        BSONObj obj() const;
        Record* rec() const;
        DeletedRecord* drec() const;
        Extent* ext() const;
        BtreeBucket* btree() const;
        BtreeBucket* btreemod() const; // marks modified / dirty

        MongoDataFile& pdf() const;
    };
#pragma pack()

    const DiskLoc minDiskLoc(0, 1);
    const DiskLoc maxDiskLoc(0x7fffffff, 0x7fffffff);

} // namespace mongo
