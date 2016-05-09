// @file file_allocator.h

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

#include <boost/filesystem/path.hpp>
#include <list>
#include <map>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

/*
 * Handles allocation of contiguous files on disk.  Allocation may be
 * requested asynchronously or synchronously.
 * singleton
 */
class FileAllocator {
    MONGO_DISALLOW_COPYING(FileAllocator);
    /*
     * The public functions may not be called concurrently.  The allocation
     * functions may be called multiple times per file, but only the first
     * size specified per file will be used.
    */
public:
    void start();

    /**
     * May be called if file exists. If file exists, or its allocation has
     *  been requested, size is updated to match existing file size.
     */
    void requestAllocation(const std::string& name, long& size);


    /**
     * Returns when file has been allocated.  If file exists, size is
     * updated to match existing file size.
     */
    void allocateAsap(const std::string& name, unsigned long long& size);

    void waitUntilFinished() const;

    static void ensureLength(int fd, long size);

    /** @return the singleton */
    static FileAllocator* get();

private:
    FileAllocator();

    void checkFailure();

    // caller must hold pendingMutex_ lock.  Returns size if allocated or
    // allocation requested, -1 otherwise.
    long prevSize(const std::string& name) const;

    // caller must hold pendingMutex_ lock.
    bool inProgress(const std::string& name) const;

    /** called from the worked thread */
    static void run(FileAllocator* fa);

    // generate a unique name for temporary files
    std::string makeTempFileName(boost::filesystem::path root);

    mutable stdx::mutex _pendingMutex;
    mutable stdx::condition_variable _pendingUpdated;

    std::list<std::string> _pending;
    mutable std::map<std::string, long> _pendingSize;

    // unique number for temporary files
    static unsigned long long _uniqueNumber;

    bool _failed;

    static FileAllocator* _instance;
};

}  // namespace mongo
