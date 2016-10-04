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
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/concurrency/rwlock.h"
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

MemoryMappedFile::MemoryMappedFile(OptionSet options)
    : MongoFile(options), _uniqueId(mmfNextId.fetchAndAdd(1)) {
    created();
}

/* Create. Must not exist.
@param zero fill file with zeros when true
*/
void* MemoryMappedFile::create(const std::string& filename, unsigned long long len, bool zero) {
    uassert(13468,
            string("can't create file already exists ") + filename,
            !boost::filesystem::exists(filename));
    void* p = map(filename.c_str(), len);
    if (p && zero) {
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

void* MemoryMappedFile::map(const char* filename) {
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
    return map(filename, l);
}

/* --- MongoFile -------------------------------------------------
   this is the administrative stuff
*/

MongoFile::MongoFile(OptionSet options)
    : _options(storageGlobalParams.readOnly ? (options | READONLY) : options) {}


RWLockRecursiveNongreedy LockMongoFilesShared::mmmutex("mmmutex", 10 * 60 * 1000 /* 10 minutes */);
unsigned LockMongoFilesShared::era = 99;  // note this rolls over

set<MongoFile*>& MongoFile::getAllFiles() {
    return mmfiles;
}

/* subclass must call in destructor (or at close).
    removes this from pathToFile and other maps
    safe to call more than once, albeit might be wasted work
    ideal to call close to the close, if the close is well before object destruction
*/
void MongoFile::destroyed() {
    LockMongoFilesShared::assertExclusivelyLocked();
    mmfiles.erase(this);
    pathToFile.erase(filename());
}

/*static*/
void MongoFile::closeAllFiles(stringstream& message) {
    static int closingAllFiles = 0;
    if (closingAllFiles) {
        message << "warning closingAllFiles=" << closingAllFiles << endl;
        return;
    }
    ++closingAllFiles;

    LockMongoFilesExclusive lk;

    ProgressMeter pm(mmfiles.size(), 2, 1, "files", "File Closing Progress");
    set<MongoFile*> temp = mmfiles;
    for (set<MongoFile*>::iterator i = temp.begin(); i != temp.end(); i++) {
        (*i)->close();  // close() now removes from mmfiles
        pm.hit();
    }
    message << "closeAllFiles() finished";
    --closingAllFiles;
}

/*static*/ long long MongoFile::totalMappedLength() {
    unsigned long long total = 0;

    LockMongoFilesShared lk;

    for (set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++)
        total += (*i)->length();

    return total;
}

/*static*/ int MongoFile::flushAll(bool sync) {
    return _flushAll(sync);
}

/*static*/ int MongoFile::_flushAll(bool sync) {
    if (!sync) {
        int num = 0;
        LockMongoFilesShared lk;
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
        LockMongoFilesShared lk;
        for (set<MongoFile*>::iterator i = mmfiles.begin(); i != mmfiles.end(); i++) {
            MongoFile* mmf = *i;
            if (!mmf)
                continue;
            thingsToFlush.push_back(mmf->prepareFlush());
        }
    }

    for (size_t i = 0; i < thingsToFlush.size(); i++) {
        thingsToFlush[i]->flush();
    }

    return thingsToFlush.size();
}

void MongoFile::created() {
    // If we're a READONLY mapping, we don't want to ever flush.
    if (!isOptionSet(READONLY)) {
        LockMongoFilesExclusive lk;
        mmfiles.insert(this);
    }
}

void MongoFile::setFilename(const std::string& fn) {
    LockMongoFilesExclusive lk;
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


void printMemInfo(const char* where) {
    LogstreamBuilder out = log();
    out << "mem info: ";
    if (where)
        out << where << " ";

    ProcessInfo pi;
    if (!pi.supported()) {
        out << " not supported";
        return;
    }

    out << "vsize: " << pi.getVirtualMemorySize() << " resident: " << pi.getResidentSize()
        << " mapped: " << (MemoryMappedFile::totalMappedLength() / (1024 * 1024));
}

void dataSyncFailedHandler() {
    log() << "error syncing data to disk, probably a disk error";
    log() << " shutting down immediately to avoid corruption";
    fassertFailed(17346);
}

}  // namespace mongo
