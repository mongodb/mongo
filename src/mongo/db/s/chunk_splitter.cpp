/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/chunk_splitter.h"

#include "mongo/db/client.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Constructs the default options for the thread pool used by the cache loader.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ChunkSplitter";
    options.minThreads = 0;
    options.maxThreads = 1;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return options;
}

}  // namespace

ChunkSplitter::ChunkSplitter() : _isPrimary(false), _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ChunkSplitter::~ChunkSplitter() {
    _threadPool.shutdown();
    _threadPool.join();
}

void ChunkSplitter::setReplicaSetMode(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    _isPrimary = isPrimary;
}

void ChunkSplitter::initiateChunkSplitter() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (_isPrimary) {
        return;
    }
    _isPrimary = true;

    log() << "The ChunkSplitter has started and will accept autosplit tasks. Any tasks that did not"
          << " have time to drain the last time this node was a primary shall be run.";
}

void ChunkSplitter::interruptChunkSplitter() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (!_isPrimary) {
        return;
    }
    _isPrimary = false;

    log() << "The ChunkSplitter has stopped and will no longer run autosplit tasks. Any autosplit "
          << "tasks that have already started will be allowed to finish.";
}

void ChunkSplitter::trySplitting(const NamespaceString& nss,
                                 const BSONObj& min,
                                 const BSONObj& max) {
    if (!_isPrimary) {
        return;
    }

    uassertStatusOK(
        _threadPool.schedule([ this, nss, min, max ]() noexcept { _runAutosplit(nss, min, max); }));
}

void ChunkSplitter::_runAutosplit(const NamespaceString& nss,
                                  const BSONObj& min,
                                  const BSONObj& max) {
    if (!_isPrimary) {
        return;
    }

    try {
        // TODO SERVER-30020
    } catch (const std::exception& e) {
        log() << "caught exception while splitting chunk: " << redact(e.what());
    }
}

}  // namespace mongo
