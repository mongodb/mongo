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

#include <boost/functional/hash.hpp>
#include <cstdint>

#include "mongo/db/jsobj.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

template <class Version>
class BtreeBucket;

#pragma pack(1)
/** represents a disk location/offset on disk in a database.  64 bits.
    it is assumed these will be passed around by value a lot so don't do anything to make them large
    (such as adding a virtual function)
 */
class DiskLoc {
    // this will be volume, file #, etc. but is a logical value could be anything depending on
    // storage engine
    int _a;
    int ofs;

public:
    enum SentinelValues {
        /* note NullOfs is different. todo clean up.  see refs to NullOfs in code - use is valid but
         * outside DiskLoc context so confusing as-is. */
        NullOfs = -1,

        // Caps the number of files that may be allocated in a database, allowing about 32TB of
        // data per db.  Note that the DiskLoc and DiskLoc56Bit types supports more files than
        // this value, as does the data storage format.
        MaxFiles = 16000,

        // How invalid DiskLocs are represented in RecordIds.
        InvalidRepr = -2LL,
    };

    DiskLoc(int a, int Ofs) : _a(a), ofs(Ofs) {}
    DiskLoc() {
        Null();
    }

    // Minimum allowed DiskLoc.  No MmapV1RecordHeader may begin at this location because file and
    // extent headers must precede Records in a file.
    static DiskLoc min() {
        return DiskLoc(0, 0);
    }

    // Maximum allowed DiskLoc.
    // No MmapV1RecordHeader may begin at this location because the minimum size of a
    // MmapV1RecordHeader is larger than one byte.  Also, the last bit is not able to be used
    // because mmapv1 uses that for "used".
    static DiskLoc max() {
        return DiskLoc(0x7fffffff, 0x7ffffffe);
    }

    bool questionable() const {
        return ofs < -1 || _a < -1 || _a > 524288;
    }

    bool isNull() const {
        return _a == -1;
    }
    DiskLoc& Null() {
        _a = -1;
        /* note NullOfs is different. todo clean up.  see refs to NullOfs in code - use is valid but
         * outside DiskLoc context so confusing as-is. */
        ofs = 0;
        return *this;
    }
    void assertOk() const {
        verify(!isNull());
    }
    DiskLoc& setInvalid() {
        _a = -2;
        ofs = 0;
        return *this;
    }
    bool isValid() const {
        return _a != -2;
    }

    std::string toString() const {
        if (isNull())
            return "null";
        std::stringstream ss;
        ss << _a << ':' << std::hex << ofs;
        return ss.str();
    }

    BSONObj toBSONObj() const {
        return BSON("file" << _a << "offset" << ofs);
    }

    int a() const {
        return _a;
    }

    int& GETOFS() {
        return ofs;
    }
    int getOfs() const {
        return ofs;
    }
    void set(int a, int b) {
        _a = a;
        ofs = b;
    }

    void inc(int amt) {
        verify(!isNull());
        ofs += amt;
    }

    bool sameFile(DiskLoc b) {
        return _a == b._a;
    }

    bool operator==(const DiskLoc& b) const {
        return _a == b._a && ofs == b.ofs;
    }
    bool operator!=(const DiskLoc& b) const {
        return !(*this == b);
    }
    int compare(const DiskLoc& b) const {
        int x = _a - b._a;
        if (x)
            return x;
        return ofs - b.ofs;
    }

    static DiskLoc fromRecordId(RecordId id) {
        if (id.isNormal())
            return DiskLoc((id.repr() >> 32), uint32_t(id.repr()));

        if (id.isNull())
            return DiskLoc();

        if (id == RecordId::max())
            return DiskLoc::max();

        if (id == RecordId::min())
            return DiskLoc::min();

        dassert(id.repr() == InvalidRepr);
        return DiskLoc().setInvalid();
    }

    RecordId toRecordId() const {
        if (_a >= 0) {
            if (*this == DiskLoc::min())
                return RecordId::min();

            if (*this == DiskLoc::max())
                return RecordId::max();

            return RecordId(uint64_t(_a) << 32 | uint32_t(ofs));
        }

        if (isNull())
            return RecordId();

        dassert(!isValid());
        return RecordId(InvalidRepr);
    }
};
#pragma pack()

inline bool operator<(const DiskLoc& rhs, const DiskLoc& lhs) {
    return rhs.compare(lhs) < 0;
}
inline bool operator<=(const DiskLoc& rhs, const DiskLoc& lhs) {
    return rhs.compare(lhs) <= 0;
}
inline bool operator>(const DiskLoc& rhs, const DiskLoc& lhs) {
    return rhs.compare(lhs) > 0;
}
inline bool operator>=(const DiskLoc& rhs, const DiskLoc& lhs) {
    return rhs.compare(lhs) >= 0;
}

inline std::ostream& operator<<(std::ostream& stream, const DiskLoc& loc) {
    return stream << loc.toString();
}

}  // namespace mongo
