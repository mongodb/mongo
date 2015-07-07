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
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/platform/atomic_word.h"

namespace mongo {

class DeletedRecord;

/* MmapV1RecordHeader is a record in a datafile.  DeletedRecord is similar but for deleted space.

*11:03:20 AM) dm10gen: regarding extentOfs...
(11:03:42 AM) dm10gen: an extent is a continugous disk area, which contains many Records and
    DeleteRecords
(11:03:56 AM) dm10gen: a DiskLoc has two pieces, the fileno and ofs.  (64 bit total)
(11:04:16 AM) dm10gen: to keep the headesr small, instead of storing a 64 bit ptr to the full extent
    address, we keep just the offset
(11:04:29 AM) dm10gen: we can do this as we know the record's address, and it has the same fileNo
(11:04:33 AM) dm10gen: see class DiskLoc for more info
(11:04:43 AM) dm10gen: so that is how MmapV1RecordHeader::myExtent() works
(11:04:53 AM) dm10gen: on an alloc(), when we build a new MmapV1RecordHeader, we must populate its
    extentOfs then
*/
#pragma pack(1)
class MmapV1RecordHeader {
public:
    enum HeaderSizeValue { HeaderSize = 16 };

    int lengthWithHeaders() const {
        return _lengthWithHeaders;
    }
    int& lengthWithHeaders() {
        return _lengthWithHeaders;
    }

    int extentOfs() const {
        return _extentOfs;
    }
    int& extentOfs() {
        return _extentOfs;
    }

    int nextOfs() const {
        return _nextOfs;
    }
    int& nextOfs() {
        return _nextOfs;
    }

    int prevOfs() const {
        return _prevOfs;
    }
    int& prevOfs() {
        return _prevOfs;
    }

    const char* data() const {
        return _data;
    }
    char* data() {
        return _data;
    }

    // XXX remove
    const char* dataNoThrowing() const {
        return _data;
    }
    char* dataNoThrowing() {
        return _data;
    }

    int netLength() const {
        return _netLength();
    }

    /* use this when a record is deleted. basically a union with next/prev fields */
    DeletedRecord& asDeleted() {
        return *((DeletedRecord*)this);
    }

    DiskLoc myExtentLoc(const DiskLoc& myLoc) const {
        return DiskLoc(myLoc.a(), extentOfs());
    }

    struct NP {
        int nextOfs;
        int prevOfs;
    };

    NP* np() {
        return (NP*)&_nextOfs;
    }

    RecordData toRecordData() const {
        return RecordData(_data, _netLength());
    }

private:
    int _netLength() const {
        return _lengthWithHeaders - HeaderSize;
    }

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
    int lengthWithHeaders() const {
        return _lengthWithHeaders;
    }
    int& lengthWithHeaders() {
        return _lengthWithHeaders;
    }

    int extentOfs() const {
        return _extentOfs;
    }
    int& extentOfs() {
        return _extentOfs;
    }

    // TODO: we need to not const_cast here but problem is DiskLoc::writing
    DiskLoc& nextDeleted() const {
        return const_cast<DiskLoc&>(_nextDeleted);
    }

private:
    int _lengthWithHeaders;

    int _extentOfs;

    DiskLoc _nextDeleted;
};

static_assert(16 == sizeof(DeletedRecord), "16 == sizeof(DeletedRecord)");

}  // namespace mongo
