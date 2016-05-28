// mmap_v1_extent_manager.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/filesystem/operations.hpp>

#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"

#include "mongo/base/counter.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/data_file.h"
#include "mongo/db/storage/mmap_v1/dur.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_options.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::endl;
using std::max;
using std::string;
using std::stringstream;

// Turn on this failpoint to force the system to yield for a fetch. Setting to "alwaysOn"
// will cause yields for fetching to occur on every 'kNeedsFetchFailFreq'th call to
// recordNeedsFetch().
static const int kNeedsFetchFailFreq = 2;
static Counter64 needsFetchFailCounter;
MONGO_FP_DECLARE(recordNeedsFetchFail);

// Used to make sure the compiler doesn't get too smart on us when we're
// trying to touch records.
volatile int __record_touch_dummy = 1;

class MmapV1RecordFetcher : public RecordFetcher {
    MONGO_DISALLOW_COPYING(MmapV1RecordFetcher);

public:
    explicit MmapV1RecordFetcher(const MmapV1RecordHeader* record) : _record(record) {}

    virtual void setup() {
        invariant(!_filesLock.get());
        _filesLock.reset(new LockMongoFilesShared());
    }

    virtual void fetch() {
        // It's only legal to touch the record while we're holding a lock on the data files.
        invariant(_filesLock.get());

        const char* recordChar = reinterpret_cast<const char*>(_record);

        // Here's where we actually deference a pointer into the record. This is where
        // we expect a page fault to occur, so we should this out of the lock.
        __record_touch_dummy += *recordChar;

        // We're not going to touch the record anymore, so we can give up our
        // lock on mongo files. We do this here because we have to release the
        // lock on mongo files prior to reacquiring lock mgr locks.
        _filesLock.reset();
    }

private:
    // The record which needs to be touched in order to page fault. Not owned by us.
    const MmapV1RecordHeader* _record;

    // This ensures that our MmapV1RecordHeader* does not drop out from under our feet before
    // we dereference it.
    std::unique_ptr<LockMongoFilesShared> _filesLock;
};

MmapV1ExtentManager::MmapV1ExtentManager(StringData dbname, StringData path, bool directoryPerDB)
    : _dbname(dbname.toString()),
      _path(path.toString()),
      _directoryPerDB(directoryPerDB),
      _rid(RESOURCE_METADATA, dbname) {
    StorageEngine* engine = getGlobalServiceContext()->getGlobalStorageEngine();
    invariant(engine->isMmapV1());
    MMAPV1Engine* mmapEngine = static_cast<MMAPV1Engine*>(engine);
    _recordAccessTracker = &mmapEngine->getRecordAccessTracker();
}

std::unique_ptr<ExtentManager> MmapV1ExtentManager::Factory::create(StringData dbname,
                                                                    StringData path,
                                                                    bool directoryPerDB) {
    return stdx::make_unique<MmapV1ExtentManager>(
        std::move(dbname), std::move(path), directoryPerDB);
}

boost::filesystem::path MmapV1ExtentManager::_fileName(int n) const {
    stringstream ss;
    ss << _dbname << '.' << n;
    boost::filesystem::path fullName(_path);
    if (_directoryPerDB)
        fullName /= _dbname;
    fullName /= ss.str();
    return fullName;
}


Status MmapV1ExtentManager::init(OperationContext* txn) {
    invariant(_files.empty());

    for (int n = 0; n < DiskLoc::MaxFiles; n++) {
        const boost::filesystem::path fullName = _fileName(n);
        if (!boost::filesystem::exists(fullName)) {
            break;
        }

        const std::string fullNameString = fullName.string();

        {
            // If the file is uninitialized we exit the loop because it is just prealloced. We
            // do this on a bare File object rather than using the DataFile because closing a
            // DataFile triggers dur::closingFileNotification() which is fatal if there are any
            // pending writes. Therefore we must only open files that we know we want to keep.
            File preview;
            preview.open(fullNameString.c_str(), /*readOnly*/ true);
            invariant(preview.is_open());

            // File can't be initialized if too small.
            if (preview.len() < sizeof(DataFileHeader)) {
                break;
            }

            // This is the equivalent of DataFileHeader::uninitialized().
            int version;
            preview.read(0, reinterpret_cast<char*>(&version), sizeof(version));
            invariant(!preview.bad());
            if (version == 0) {
                break;
            }
        }

        unique_ptr<DataFile> df(new DataFile(n));

        Status s = df->openExisting(fullNameString.c_str());
        if (!s.isOK()) {
            return s;
        }

        invariant(!df->getHeader()->uninitialized());

        // We only checkUpgrade on files that we are keeping, not preallocs.
        df->getHeader()->checkUpgrade(txn);

        _files.push_back(df.release());
    }

    // If this is a new database being created, instantiate the first file and one extent so
    // we can have a coherent database.
    if (_files.empty()) {
        WriteUnitOfWork wuow(txn);
        _createExtent(txn, initialSize(128), false);
        wuow.commit();

        // Commit the journal and all changes to disk so that even if exceptions occur during
        // subsequent initialization, we won't have uncommited changes during file close.
        getDur().commitNow(txn);
    }

    return Status::OK();
}

const DataFile* MmapV1ExtentManager::_getOpenFile(int fileId) const {
    if (fileId < 0 || fileId >= _files.size()) {
        log() << "_getOpenFile() invalid file index requested " << fileId;
        invariant(false);
    }

    return _files[fileId];
}

DataFile* MmapV1ExtentManager::_getOpenFile(int fileId) {
    if (fileId < 0 || fileId >= _files.size()) {
        log() << "_getOpenFile() invalid file index requested " << fileId;
        invariant(false);
    }

    return _files[fileId];
}

DataFile* MmapV1ExtentManager::_addAFile(OperationContext* txn,
                                         int sizeNeeded,
                                         bool preallocateNextFile) {
    // Database must be stable and we need to be in some sort of an update operation in order
    // to add a new file.
    invariant(txn->lockState()->isDbLockedForMode(_dbname, MODE_IX));

    const int allocFileId = _files.size();

    int minSize = 0;
    if (allocFileId > 0) {
        // Make the next file at least as large as the previous
        minSize = _files[allocFileId - 1]->getHeader()->fileLength;
    }

    if (minSize < sizeNeeded + DataFileHeader::HeaderSize) {
        minSize = sizeNeeded + DataFileHeader::HeaderSize;
    }

    {
        unique_ptr<DataFile> allocFile(new DataFile(allocFileId));
        const string allocFileName = _fileName(allocFileId).string();

        Timer t;

        allocFile->open(txn, allocFileName.c_str(), minSize, false);
        if (t.seconds() > 1) {
            log() << "MmapV1ExtentManager took " << t.seconds()
                  << " seconds to open: " << allocFileName;
        }

        // It's all good
        _files.push_back(allocFile.release());
    }

    // Preallocate is asynchronous
    if (preallocateNextFile) {
        unique_ptr<DataFile> nextFile(new DataFile(allocFileId + 1));
        const string nextFileName = _fileName(allocFileId + 1).string();

        nextFile->open(txn, nextFileName.c_str(), minSize, false);
    }

    // Returns the last file added
    return _files[allocFileId];
}

int MmapV1ExtentManager::numFiles() const {
    return _files.size();
}

long long MmapV1ExtentManager::fileSize() const {
    long long size = 0;
    for (int n = 0; boost::filesystem::exists(_fileName(n)); n++) {
        size += boost::filesystem::file_size(_fileName(n));
    }

    return size;
}

MmapV1RecordHeader* MmapV1ExtentManager::_recordForV1(const DiskLoc& loc) const {
    loc.assertOk();
    const DataFile* df = _getOpenFile(loc.a());

    int ofs = loc.getOfs();
    if (ofs < DataFileHeader::HeaderSize) {
        df->badOfs(ofs);  // will msgassert - external call to keep out of the normal code path
    }

    return reinterpret_cast<MmapV1RecordHeader*>(df->p() + ofs);
}

MmapV1RecordHeader* MmapV1ExtentManager::recordForV1(const DiskLoc& loc) const {
    MmapV1RecordHeader* record = _recordForV1(loc);
    _recordAccessTracker->markAccessed(record);
    return record;
}

std::unique_ptr<RecordFetcher> MmapV1ExtentManager::recordNeedsFetch(const DiskLoc& loc) const {
    if (loc.isNull())
        return {};
    MmapV1RecordHeader* record = _recordForV1(loc);

    // For testing: if failpoint is enabled we randomly request fetches without
    // going to the RecordAccessTracker.
    if (MONGO_FAIL_POINT(recordNeedsFetchFail)) {
        needsFetchFailCounter.increment();
        if ((needsFetchFailCounter.get() % kNeedsFetchFailFreq) == 0) {
            return stdx::make_unique<MmapV1RecordFetcher>(record);
        }
    }

    if (!_recordAccessTracker->checkAccessedAndMark(record)) {
        return stdx::make_unique<MmapV1RecordFetcher>(record);
    }

    return {};
}

DiskLoc MmapV1ExtentManager::extentLocForV1(const DiskLoc& loc) const {
    MmapV1RecordHeader* record = recordForV1(loc);
    return DiskLoc(loc.a(), record->extentOfs());
}

Extent* MmapV1ExtentManager::extentForV1(const DiskLoc& loc) const {
    DiskLoc extentLoc = extentLocForV1(loc);
    return getExtent(extentLoc);
}

Extent* MmapV1ExtentManager::getExtent(const DiskLoc& loc, bool doSanityCheck) const {
    loc.assertOk();
    Extent* e = reinterpret_cast<Extent*>(_getOpenFile(loc.a())->p() + loc.getOfs());
    if (doSanityCheck)
        e->assertOk();

    _recordAccessTracker->markAccessed(e);

    return e;
}

void _checkQuota(bool enforceQuota, int fileNo) {
    if (!enforceQuota)
        return;

    if (fileNo < mmapv1GlobalOptions.quotaFiles)
        return;

    uasserted(12501, "quota exceeded");
}

int MmapV1ExtentManager::maxSize() const {
    return DataFile::maxSize() - DataFileHeader::HeaderSize - 16;
}

DiskLoc MmapV1ExtentManager::_createExtentInFile(
    OperationContext* txn, int fileNo, DataFile* f, int size, bool enforceQuota) {
    _checkQuota(enforceQuota, fileNo - 1);

    massert(10358, "bad new extent size", size >= minSize() && size <= maxSize());

    DiskLoc loc = f->allocExtentArea(txn, size);
    loc.assertOk();

    Extent* e = getExtent(loc, false);
    verify(e);

    *txn->recoveryUnit()->writing(&e->magic) = Extent::extentSignature;
    *txn->recoveryUnit()->writing(&e->myLoc) = loc;
    *txn->recoveryUnit()->writing(&e->length) = size;

    return loc;
}


DiskLoc MmapV1ExtentManager::_createExtent(OperationContext* txn, int size, bool enforceQuota) {
    size = quantizeExtentSize(size);

    if (size > maxSize())
        size = maxSize();

    verify(size < DataFile::maxSize());

    for (int i = numFiles() - 1; i >= 0; i--) {
        DataFile* f = _getOpenFile(i);
        invariant(f);

        if (f->getHeader()->unusedLength >= size) {
            return _createExtentInFile(txn, i, f, size, enforceQuota);
        }
    }

    _checkQuota(enforceQuota, numFiles());

    // no space in an existing file
    // allocate files until we either get one big enough or hit maxSize
    for (int i = 0; i < 8; i++) {
        DataFile* f = _addAFile(txn, size, false);

        if (f->getHeader()->unusedLength >= size) {
            return _createExtentInFile(txn, numFiles() - 1, f, size, enforceQuota);
        }
    }

    // callers don't check for null return code, so assert
    msgasserted(14810, "couldn't allocate space for a new extent");
}

DiskLoc MmapV1ExtentManager::_allocFromFreeList(OperationContext* txn,
                                                int approxSize,
                                                bool capped) {
    // setup extent constraints

    int low, high;
    if (capped) {
        // be strict about the size
        low = approxSize;
        if (low > 2048)
            low -= 256;
        high = (int)(approxSize * 1.05) + 256;
    } else {
        low = (int)(approxSize * 0.8);
        high = (int)(approxSize * 1.4);
    }
    if (high <= 0) {
        // overflowed
        high = max(approxSize, maxSize());
    }
    if (high <= minSize()) {
        // the minimum extent size is 4097
        high = minSize() + 1;
    }

    // scan free list looking for something suitable

    int n = 0;
    Extent* best = 0;
    int bestDiff = 0x7fffffff;
    {
        Timer t;
        DiskLoc L = _getFreeListStart();
        while (!L.isNull()) {
            Extent* e = getExtent(L);
            if (e->length >= low && e->length <= high) {
                int diff = abs(e->length - approxSize);
                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = e;
                    if (((double)diff) / approxSize < 0.1) {
                        // close enough
                        break;
                    }
                    if (t.seconds() >= 2) {
                        // have spent lots of time in write lock, and we are in [low,high], so close
                        // enough could come into play if extent freelist is very long
                        break;
                    }
                } else {
                    OCCASIONALLY {
                        if (high < 64 * 1024 && t.seconds() >= 2) {
                            // be less picky if it is taking a long time
                            high = 64 * 1024;
                        }
                    }
                }
            }
            L = e->xnext;
            ++n;
        }
        if (t.seconds() >= 10) {
            log() << "warning: slow scan in allocFromFreeList (in write lock)" << endl;
        }
    }

    if (n > 128) {
        LOG(n < 512 ? 1 : 0) << "warning: newExtent " << n << " scanned\n";
    }

    if (!best)
        return DiskLoc();

    // remove from the free list
    if (!best->xprev.isNull())
        *txn->recoveryUnit()->writing(&getExtent(best->xprev)->xnext) = best->xnext;
    if (!best->xnext.isNull())
        *txn->recoveryUnit()->writing(&getExtent(best->xnext)->xprev) = best->xprev;
    if (_getFreeListStart() == best->myLoc)
        _setFreeListStart(txn, best->xnext);
    if (_getFreeListEnd() == best->myLoc)
        _setFreeListEnd(txn, best->xprev);

    return best->myLoc;
}

DiskLoc MmapV1ExtentManager::allocateExtent(OperationContext* txn,
                                            bool capped,
                                            int size,
                                            bool enforceQuota) {
    Lock::ResourceLock rlk(txn->lockState(), _rid, MODE_X);
    bool fromFreeList = true;
    DiskLoc eloc = _allocFromFreeList(txn, size, capped);
    if (eloc.isNull()) {
        fromFreeList = false;
        eloc = _createExtent(txn, size, enforceQuota);
    }

    invariant(!eloc.isNull());
    invariant(eloc.isValid());

    LOG(1) << "MmapV1ExtentManager::allocateExtent"
           << " desiredSize:" << size << " fromFreeList: " << fromFreeList << " eloc: " << eloc;

    return eloc;
}

void MmapV1ExtentManager::freeExtent(OperationContext* txn, DiskLoc firstExt) {
    Lock::ResourceLock rlk(txn->lockState(), _rid, MODE_X);
    Extent* e = getExtent(firstExt);
    txn->recoveryUnit()->writing(&e->xnext)->Null();
    txn->recoveryUnit()->writing(&e->xprev)->Null();
    txn->recoveryUnit()->writing(&e->firstRecord)->Null();
    txn->recoveryUnit()->writing(&e->lastRecord)->Null();


    if (_getFreeListStart().isNull()) {
        _setFreeListStart(txn, firstExt);
        _setFreeListEnd(txn, firstExt);
    } else {
        DiskLoc a = _getFreeListStart();
        invariant(getExtent(a)->xprev.isNull());
        *txn->recoveryUnit()->writing(&getExtent(a)->xprev) = firstExt;
        *txn->recoveryUnit()->writing(&getExtent(firstExt)->xnext) = a;
        _setFreeListStart(txn, firstExt);
    }
}

void MmapV1ExtentManager::freeExtents(OperationContext* txn, DiskLoc firstExt, DiskLoc lastExt) {
    Lock::ResourceLock rlk(txn->lockState(), _rid, MODE_X);

    if (firstExt.isNull() && lastExt.isNull())
        return;

    {
        verify(!firstExt.isNull() && !lastExt.isNull());
        Extent* f = getExtent(firstExt);
        Extent* l = getExtent(lastExt);
        verify(f->xprev.isNull());
        verify(l->xnext.isNull());
        verify(f == l || !f->xnext.isNull());
        verify(f == l || !l->xprev.isNull());
    }

    if (_getFreeListStart().isNull()) {
        _setFreeListStart(txn, firstExt);
        _setFreeListEnd(txn, lastExt);
    } else {
        DiskLoc a = _getFreeListStart();
        invariant(getExtent(a)->xprev.isNull());
        *txn->recoveryUnit()->writing(&getExtent(a)->xprev) = lastExt;
        *txn->recoveryUnit()->writing(&getExtent(lastExt)->xnext) = a;
        _setFreeListStart(txn, firstExt);
    }
}

DiskLoc MmapV1ExtentManager::_getFreeListStart() const {
    if (_files.empty())
        return DiskLoc();
    const DataFile* file = _getOpenFile(0);
    return file->header()->freeListStart;
}

DiskLoc MmapV1ExtentManager::_getFreeListEnd() const {
    if (_files.empty())
        return DiskLoc();
    const DataFile* file = _getOpenFile(0);
    return file->header()->freeListEnd;
}

void MmapV1ExtentManager::_setFreeListStart(OperationContext* txn, DiskLoc loc) {
    invariant(!_files.empty());
    DataFile* file = _files[0];
    *txn->recoveryUnit()->writing(&file->header()->freeListStart) = loc;
}

void MmapV1ExtentManager::_setFreeListEnd(OperationContext* txn, DiskLoc loc) {
    invariant(!_files.empty());
    DataFile* file = _files[0];
    *txn->recoveryUnit()->writing(&file->header()->freeListEnd) = loc;
}

void MmapV1ExtentManager::freeListStats(OperationContext* txn,
                                        int* numExtents,
                                        int64_t* totalFreeSizeBytes) const {
    Lock::ResourceLock rlk(txn->lockState(), _rid, MODE_S);

    invariant(numExtents);
    invariant(totalFreeSizeBytes);

    *numExtents = 0;
    *totalFreeSizeBytes = 0;

    DiskLoc a = _getFreeListStart();
    while (!a.isNull()) {
        Extent* e = getExtent(a);
        (*numExtents)++;
        (*totalFreeSizeBytes) += e->length;
        a = e->xnext;
    }
}

void MmapV1ExtentManager::printFreeList() const {
    log() << "dump freelist " << _dbname << endl;

    DiskLoc a = _getFreeListStart();
    while (!a.isNull()) {
        Extent* e = getExtent(a);
        log() << "  extent " << a.toString() << " len:" << e->length
              << " prev:" << e->xprev.toString() << endl;
        a = e->xnext;
    }

    log() << "end freelist" << endl;
}

namespace {
class CacheHintMadvise : public ExtentManager::CacheHint {
public:
    CacheHintMadvise(void* p, unsigned len, MAdvise::Advice a) : _advice(p, len, a) {}

private:
    MAdvise _advice;
};
}

ExtentManager::CacheHint* MmapV1ExtentManager::cacheHint(const DiskLoc& extentLoc,
                                                         const ExtentManager::HintType& hint) {
    invariant(hint == Sequential);
    Extent* e = getExtent(extentLoc);
    return new CacheHintMadvise(reinterpret_cast<void*>(e), e->length, MAdvise::Sequential);
}

MmapV1ExtentManager::FilesArray::~FilesArray() {
    for (int i = 0; i < size(); i++) {
        delete _files[i];
    }
}

void MmapV1ExtentManager::FilesArray::push_back(DataFile* val) {
    stdx::lock_guard<stdx::mutex> lk(_writersMutex);
    const int n = _size.load();
    invariant(n < DiskLoc::MaxFiles);
    // Note ordering: _size update must come after updating the _files array
    _files[n] = val;
    _size.store(n + 1);
}

DataFileVersion MmapV1ExtentManager::getFileFormat(OperationContext* txn) const {
    if (numFiles() == 0)
        return DataFileVersion(0, 0);

    // We explicitly only look at the first file.
    return _getOpenFile(0)->getHeader()->version;
}

void MmapV1ExtentManager::setFileFormat(OperationContext* txn, DataFileVersion newVersion) {
    invariant(numFiles() > 0);

    DataFile* df = _getOpenFile(0);
    invariant(df);

    *txn->recoveryUnit()->writing(&df->getHeader()->version) = newVersion;
}
}
