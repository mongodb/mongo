// data_file.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/data_file.h"

#include <boost/filesystem/operations.hpp>
#include <utility>
#include <vector>

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/durable_mapped_file.h"
#include "mongo/db/storage/mmap_v1/file_allocator.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;

namespace {

void data_file_check(void* _mb) {
    if (sizeof(char*) == 4) {
        uassert(10084,
                "can't map file memory - mongo requires 64 bit build for larger datasets",
                _mb != NULL);
    } else {
        uassert(10085, "can't map file memory", _mb != NULL);
    }
}

}  // namespace


static_assert(DataFileHeader::HeaderSize == 8192, "DataFileHeader::HeaderSize == 8192");
static_assert(sizeof(static_cast<DataFileHeader*>(NULL)->data) == 4,
              "sizeof(static_cast<DataFileHeader*>(NULL)->data) == 4");
static_assert(sizeof(DataFileHeader) - sizeof(static_cast<DataFileHeader*>(NULL)->data) ==
                  DataFileHeader::HeaderSize,
              "sizeof(DataFileHeader) - sizeof(static_cast<DataFileHeader*>(NULL)->data) == "
              "DataFileHeader::HeaderSize");


int DataFile::maxSize() {
    if (sizeof(int*) == 4) {
        return 512 * 1024 * 1024;
    } else if (mmapv1GlobalOptions.smallfiles) {
        return 0x7ff00000 >> 2;
    } else {
        return 0x7ff00000;
    }
}

NOINLINE_DECL void DataFile::badOfs(int ofs) const {
    msgasserted(13440,
                str::stream() << "bad offset:" << ofs << " accessing file: " << mmf.filename()
                              << ". See http://dochub.mongodb.org/core/data-recovery");
}

int DataFile::_defaultSize() const {
    int size;

    if (_fileNo <= 4) {
        size = (64 * 1024 * 1024) << _fileNo;
    } else {
        size = 0x7ff00000;
    }

    if (mmapv1GlobalOptions.smallfiles) {
        size = size >> 2;
    }

    return size;
}

/** @return true if found and opened. if uninitialized (prealloc only) does not open. */
Status DataFile::openExisting(const char* filename) {
    invariant(_mb == 0);

    if (!boost::filesystem::exists(filename)) {
        return Status(ErrorCodes::InvalidPath, "DataFile::openExisting - file does not exist");
    }

    if (!mmf.open(filename)) {
        return Status(ErrorCodes::InternalError, "DataFile::openExisting - mmf.open failed");
    }

    // The mapped view of the file should never be NULL if the open call above succeeded.
    _mb = mmf.getView();
    invariant(_mb);

    const uint64_t sz = mmf.length();
    invariant(sz <= 0x7fffffff);
    invariant(sz % 4096 == 0);

    if (sz < 64 * 1024 * 1024 && !mmapv1GlobalOptions.smallfiles) {
        if (sz >= 16 * 1024 * 1024 && sz % (1024 * 1024) == 0) {
            log() << "info openExisting file size " << sz
                  << " but mmapv1GlobalOptions.smallfiles=false: " << filename << endl;
        } else {
            log() << "openExisting size " << sz << " less than minimum file size expectation "
                  << filename << endl;
            verify(false);
        }
    }

    data_file_check(_mb);
    return Status::OK();
}

void DataFile::open(OperationContext* txn,
                    const char* filename,
                    int minSize,
                    bool preallocateOnly) {
    long size = _defaultSize();

    while (size < minSize) {
        if (size < maxSize() / 2) {
            size *= 2;
        } else {
            size = maxSize();
            break;
        }
    }

    if (size > maxSize()) {
        size = maxSize();
    }

    invariant(size >= 64 * 1024 * 1024 || mmapv1GlobalOptions.smallfiles);
    invariant(size % 4096 == 0);

    if (preallocateOnly) {
        if (mmapv1GlobalOptions.prealloc) {
            FileAllocator::get()->requestAllocation(filename, size);
        }
        return;
    }

    {
        invariant(_mb == 0);
        unsigned long long sz = size;
        if (mmf.create(filename, sz)) {
            _mb = mmf.getView();
        }

        invariant(sz <= 0x7fffffff);
        size = (int)sz;
    }

    data_file_check(_mb);
    header()->init(txn, _fileNo, size, filename);
}

void DataFile::flush(bool sync) {
    mmf.flush(sync);
}

DiskLoc DataFile::allocExtentArea(OperationContext* txn, int size) {
    // The header would be NULL if file open failed. However, if file open failed we should
    // never be entering here.
    invariant(header());
    invariant(size <= header()->unusedLength);

    int offset = header()->unused.getOfs();

    DataFileHeader* h = header();
    *txn->recoveryUnit()->writing(&h->unused) = DiskLoc(_fileNo, offset + size);
    txn->recoveryUnit()->writingInt(h->unusedLength) = h->unusedLength - size;

    return DiskLoc(_fileNo, offset);
}

// -------------------------------------------------------------------------------

void DataFileHeader::init(OperationContext* txn, int fileno, int filelength, const char* filename) {
    if (uninitialized()) {
        DEV log() << "datafileheader::init initializing " << filename << " n:" << fileno << endl;

        massert(13640,
                str::stream() << "DataFileHeader looks corrupt at file open filelength:"
                              << filelength
                              << " fileno:"
                              << fileno,
                filelength > 32768);

        // The writes done in this function must not be rolled back. If the containing
        // UnitOfWork rolls back it should roll back to the state *after* these writes. This
        // will leave the file empty, but available for future use. That is why we go directly
        // to the global dur dirty list rather than going through the RecoveryUnit.
        getDur().createdFile(filename, filelength);

        typedef std::pair<void*, unsigned> Intent;
        std::vector<Intent> intent;
        intent.push_back(std::make_pair(this, sizeof(DataFileHeader)));
        privateViews.makeWritable(this, sizeof(DataFileHeader));
        getDur().declareWriteIntents(intent);

        fileLength = filelength;
        version = DataFileVersion::defaultForNewFiles();
        unused.set(fileno, HeaderSize);
        unusedLength = fileLength - HeaderSize - 16;
        freeListStart.Null();
        freeListEnd.Null();
    } else {
        checkUpgrade(txn);
    }
}

void DataFileHeader::checkUpgrade(OperationContext* txn) {
    if (freeListStart == DiskLoc(0, 0)) {
        // we are upgrading from 2.4 to 2.6
        invariant(freeListEnd == DiskLoc(0, 0));  // both start and end should be (0,0) or real
        WriteUnitOfWork wunit(txn);
        *txn->recoveryUnit()->writing(&freeListStart) = DiskLoc();
        *txn->recoveryUnit()->writing(&freeListEnd) = DiskLoc();
        wunit.commit();
    }
}
}
