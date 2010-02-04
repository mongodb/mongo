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

/* storage.h

   Storage subsystem management.
   Lays out our datafiles on disk, manages disk space.
*/

#pragma once

namespace mongo {

#pragma pack(1)

    class Record;
    class DeletedRecord;
    class Extent;
    class BtreeBucket;
    class BSONObj;
    class MongoDataFile;

    class DiskLoc {
        int fileNo; /* this will be volume, file #, etc. */
        int ofs;
    public:
        // Note: MaxFiles imposes a limit of about 32TB of data per process
        enum SentinelValues { MaxFiles=16000, NullOfs = -1 };

        int a() const {
            return fileNo;
        }

        DiskLoc(int a, int b) : fileNo(a), ofs(b) {
            //assert(ofs!=0);
        }
        DiskLoc() { Null(); }
        DiskLoc(const DiskLoc& l) {
            fileNo=l.fileNo;
            ofs=l.ofs;
        }

        bool questionable() {
            return ofs < -1 ||
                   fileNo < -1 ||
                   fileNo > 524288;
        }

        bool isNull() const {
            return fileNo == -1;
            //            return ofs == NullOfs;
        }
        void Null() {
            fileNo = -1;
            ofs = 0;
        }
        void assertOk() {
            assert(!isNull());
        }
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
        operator string() const { return toString(); }

        int& GETOFS() {
            return ofs;
        }
        int getOfs() const {
            return ofs;
        }
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

        /* get the "thing" associated with this disk location.
           it is assumed the object is what it is -- you must asure that:
           think of this as an unchecked type cast.
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
