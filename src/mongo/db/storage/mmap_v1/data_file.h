// data_file.h

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

#include "mongo/bson/util/builder.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/platform/bits.h"

namespace mongo {

class OperationContext;

#pragma pack(1)
class DataFileVersion {
public:
    DataFileVersion(uint32_t major, uint32_t minor) : _major(major), _minor(minor) {}

    static DataFileVersion defaultForNewFiles() {
        return DataFileVersion(kCurrentMajor, kIndexes24AndNewer | kMayHave30Freelist);
    }

    Status isCompatibleWithCurrentCode() const {
        if (_major != kCurrentMajor) {
            StringBuilder sb;
            sb << "The data files have major version " << _major
               << ", but this version of mongod only supports version " << kCurrentMajor;
            return {ErrorCodes::MustUpgrade, sb.str()};
        }

        uint32_t unrecognizedMinorBits = _minor & ~kUsedMinorFlagsMask;
        if (unrecognizedMinorBits) {
            StringBuilder sb;
            sb << "The data files use features not recognized by this version of mongod; the"
                  " feature bits in positions [ ";
            bool firstIteration = true;
            while (unrecognizedMinorBits) {
                const int lowestSetBitPosition = countTrailingZeros64(unrecognizedMinorBits);
                if (!firstIteration) {
                    sb << ", ";
                }
                sb << lowestSetBitPosition;
                unrecognizedMinorBits ^= (1 << lowestSetBitPosition);
                firstIteration = false;
            }
            sb << " ] aren't recognized by this version of mongod";

            return {ErrorCodes::MustUpgrade, sb.str()};
        }

        const uint32_t indexCleanliness = _minor & kIndexPluginMask;
        if (indexCleanliness != kIndexes24AndNewer && indexCleanliness != kIndexes22AndOlder) {
            StringBuilder sb;
            sb << "The data files have index plugin version " << indexCleanliness
               << ", but this version of mongod only supports versions " << kIndexes22AndOlder
               << " and " << kIndexes24AndNewer;
            return {ErrorCodes::MustUpgrade, sb.str()};
        }

        // We are compatible with either setting of kMayHave30Freelist.

        return Status::OK();
    }

    bool is24IndexClean() const {
        return (_minor & kIndexPluginMask) == kIndexes24AndNewer;
    }
    void setIs24IndexClean() {
        _minor = ((_minor & ~kIndexPluginMask) | kIndexes24AndNewer);
    }

    bool mayHave30Freelist() const {
        return _minor & kMayHave30Freelist;
    }
    void setMayHave30Freelist() {
        _minor |= kMayHave30Freelist;
    }

    bool getMayHaveCollationMetadata() const {
        return _minor & kMayHaveCollationMetadata;
    }
    void setMayHaveCollationMetadata() {
        _minor |= kMayHaveCollationMetadata;
    }

    uint32_t majorRaw() const {
        return _major;
    }
    uint32_t minorRaw() const {
        return _minor;
    }

private:
    static const uint32_t kCurrentMajor = 4;

    // minor layout:
    // first 4 bits - index plugin cleanliness.
    //    see IndexCatalog::_upgradeDatabaseMinorVersionIfNeeded for details
    // 5th bit - 1 if started with 3.0-style freelist implementation (SERVER-14081)
    // 6th bit - 1 if indexes or collections with a collation have been created.
    // 7th through 31st bit - reserved and must be set to 0.
    static const uint32_t kIndexPluginMask = 0xf;
    static const uint32_t kIndexes22AndOlder = 5;
    static const uint32_t kIndexes24AndNewer = 6;

    static const uint32_t kMayHave30Freelist = (1 << 4);

    static const uint32_t kMayHaveCollationMetadata = (1 << 5);

    // All set bits we know about are covered by this mask.
    static const uint32_t kUsedMinorFlagsMask =
        kIndexPluginMask | kMayHave30Freelist | kMayHaveCollationMetadata;

    uint32_t _major;
    uint32_t _minor;
};

// Note: Intentionally not defining relational operators for DataFileVersion as there is no
// total ordering of all versions now that '_minor' is used as a bit vector.
#pragma pack()

/*  a datafile - i.e. the "dbname.<#>" files :

      ----------------------
      DataFileHeader
      ----------------------
      Extent (for a particular namespace)
        MmapV1RecordHeader
        ...
        MmapV1RecordHeader (some chained for unused space)
      ----------------------
      more Extents...
      ----------------------
*/
#pragma pack(1)
class DataFileHeader {
public:
    DataFileVersion version;
    int fileLength;
    /**
     * unused is the portion of the file that doesn't belong to any allocated extents. -1 = no more
     */
    DiskLoc unused;
    int unusedLength;
    DiskLoc freeListStart;
    DiskLoc freeListEnd;
    char reserved[8192 - 4 * 4 - 8 * 3];

    char data[4];  // first extent starts here

    enum { HeaderSize = 8192 };

    bool uninitialized() const {
        return version.majorRaw() == 0;
    }

    void init(OperationContext* txn, int fileno, int filelength, const char* filename);

    void checkUpgrade(OperationContext* txn);

    bool isEmpty() const {
        return uninitialized() || (unusedLength == fileLength - HeaderSize - 16);
    }
};
#pragma pack()


class DataFile {
public:
    DataFile(int fn) : _fileNo(fn), _mb(NULL) {}

    /** @return true if found and opened. if uninitialized (prealloc only) does not open. */
    Status openExisting(const char* filename);

    /** creates if DNE */
    void open(OperationContext* txn,
              const char* filename,
              int requestedDataSize = 0,
              bool preallocateOnly = false);

    DiskLoc allocExtentArea(OperationContext* txn, int size);

    DataFileHeader* getHeader() {
        return header();
    }
    const DataFileHeader* getHeader() const {
        return header();
    }

    HANDLE getFd() {
        return mmf.getFd();
    }
    unsigned long long length() const {
        return mmf.length();
    }

    /* return max size an extent may be */
    static int maxSize();

    /** fsync */
    void flush(bool sync);

private:
    friend class MmapV1ExtentManager;


    void badOfs(int) const;
    int _defaultSize() const;

    void grow(DiskLoc dl, int size);

    char* p() const {
        return (char*)_mb;
    }
    DataFileHeader* header() {
        return static_cast<DataFileHeader*>(_mb);
    }
    const DataFileHeader* header() const {
        return static_cast<DataFileHeader*>(_mb);
    }


    const int _fileNo;

    DurableMappedFile mmf;
    void* _mb;  // the memory mapped view
};
}
