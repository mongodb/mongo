// mmap.cpp

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/mmap.h"

#include <boost/filesystem/operations.hpp>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/startup_test.h"

namespace mongo {

using std::endl;
using std::map;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

void minOSPageSizeBytesTest(size_t minOSPageSizeBytes) {
    fassert(16325, minOSPageSizeBytes > 0);
    fassert(16326, minOSPageSizeBytes < 1000000);
    // check to see if the page size is a power of 2
    fassert(16327, (minOSPageSizeBytes & (minOSPageSizeBytes - 1)) == 0);
}

namespace {
set<MongoFile*> mmfiles;
map<string, MongoFile*> pathToFile;
mongo::AtomicUInt64 mmfNextId(0);
}  // namespace

MemoryMappedFile::MemoryMappedFile(OperationContext* txn, OptionSet options)
    : MongoFile(options), _uniqueId(mmfNextId.fetchAndAdd(1)) {
    created(txn);
}

MemoryMappedFile::~MemoryMappedFile() {
    invariant(isClosed());

    auto txn = cc().getOperationContext();
    invariant(txn);

    LockMongoFilesShared lock(txn);
    for (std::set<MongoFile*>::const_iterator it = mmfiles.begin(); it != mmfiles.end(); it++) {
        invariant(*it != this);
    }
}

/*static*/ AtomicUInt64 MemoryMappedFile::totalMappedLength;

void* MemoryMappedFile::create(OperationContext* txn,
                               const std::string& filename,
                               unsigned long long len,
                               bool zero) {
    uassert(13468,
            string("can't create file already exists ") + filename,
            !boost::filesystem::exists(filename));
    void* p = map(txn, filename.c_str(), len);
    fassert(16331, p);
    if (zero) {
        size_t sz = (size_t)len;
        verify(len == sz);
        memset(p, 0, sz);
    }
    return p;
}

/*static*/ void MemoryMappedFile::updateLength(const char* filename, unsigned long long& length) {
    if (!boost::filesystem::exists(filename))
        return;
    // make sure we map full length if preexisting file.
    boost::uintmax_t l = boost::filesystem::file_size(filename);
    length = l;
}

void* MemoryMappedFile::map(OperationContext* txn, const char* filename) {
    unsigned long long l;
    try {
        l = boost::filesystem::file_size(filename);
    } catch (boost::filesystem::filesystem_error& e) {
        uasserted(15922,
                  mongoutils::str::stream() << "couldn't get file length when opening mapping "
                                            << filename
                                            << ' '
                                            << e.what());
    }

    void* ret = map(txn, filename, l);
    fassert(16334, ret);
    return ret;
}

/* --- MongoFile -------------------------------------------------
   this is the administrative stuff
*/

MongoFile::MongoFile(OptionSet options)
    : _options(storageGlobalParams.readOnly ? (options | READONLY) : options) {}


Lock::ResourceMutex LockMongoFilesShared::mmmutex("MMapMutex");
unsigned LockMongoFilesShared::era = 99;  // note this rolls over

set<MongoFile*>& MongoFile::getAllFiles() {
    return mmfiles;
}

/* subclass must call in destructor (or at close).
    removes this from pathToFile and other maps
    safe to call more than once, albeit might be wasted work
    ideal to call close to the close, if the close is well before object destruction
*/
void MongoFile::destroyed(OperationContext* txn) {
    LockMongoFilesShared::assertExclusivelyLocked(txn);
    mmfiles.erase(this);
    pathToFile.erase(filename());
}

/*static*/
void MongoFile::closeAllFiles(OperationContext* txn, stringstream& message) {
    static int closingAllFiles = 0;
    if (closingAllFiles) {
        message << "warning closingAllFiles=" << closingAllFiles << endl;
        return;
    }
    ++closingAllFiles;

    LockMongoFilesExclusive lk(txn);

    ProgressMeter pm(mmfiles.size(), 2, 1, "files", "File Closing Progress");
    set<MongoFile*> temp = mmfiles;
    for (set<MongoFile*>::iterator i = temp.begin(); i != temp.end(); i++) {
        (*i)->close(txn);  // close() now removes from mmfiles
        pm.hit();
    }
    message << "closeAllFiles() finished";
    --closingAllFiles;
}

/*static*/ int MongoFile::flushAll(OperationContext* txn, bool sync) {
    return _flushAll(txn, sync);
}

/*static*/ int MongoFile::_flushAll(OperationContext* txn, bool sync) {
    if (!sync) {
        int num = 0;
        LockMongoFilesShared lk(txn);
        for (set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++) {
            num++;
            MongoFile* mmf = *i;
            if (!mmf)
                continue;

            invariant(!mmf->isOptionSet(READONLY));
            mmf->flush(sync);
        }
        return num;
    }

    // want to do it sync

    // get a thread-safe Flushable object for each file first in a single lock
    // so that we can iterate and flush without doing any locking here
    OwnedPointerVector<Flushable> thingsToFlushWrapper;
    vector<Flushable*>& thingsToFlush = thingsToFlushWrapper.mutableVector();
    {
        LockMongoFilesShared lk(txn);
        for (set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++) {
            MongoFile* mmf = *i;
            if (!mmf)
                continue;
            thingsToFlush.push_back(mmf->prepareFlush());
        }
    }

    for (size_t i = 0; i < thingsToFlush.size(); i++) {
        thingsToFlush[i]->flush(txn);
    }

    return thingsToFlush.size();
}

void MongoFile::created(OperationContext* txn) {
    // If we're a READONLY mapping, we don't want to ever flush.
    if (!isOptionSet(READONLY)) {
        LockMongoFilesExclusive lk(txn);
        mmfiles.insert(this);
    }
}

void MongoFile::setFilename(OperationContext* txn, const std::string& fn) {
    LockMongoFilesExclusive lk(txn);
    verify(_filename.empty());
    _filename = boost::filesystem::absolute(fn).generic_string();
    MongoFile*& ptf = pathToFile[_filename];
    massert(13617, "MongoFile : multiple opens of same filename", ptf == 0);
    ptf = this;
}

MongoFile* MongoFileFinder::findByPath(const std::string& path) const {
    return mapFindWithDefault(pathToFile,
                              boost::filesystem::absolute(path).generic_string(),
                              static_cast<MongoFile*>(NULL));
}

void dataSyncFailedHandler() {
    log() << "error syncing data to disk, probably a disk error";
    log() << " shutting down immediately to avoid corruption";
    fassertFailed(17346);
}

}  // namespace mongo
