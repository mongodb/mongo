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

#pragma once

#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class NamespaceString;

/**
 * Handles asynchronous auto-splitting of chunks.
 */
class ChunkSplitter {
    MONGO_DISALLOW_COPYING(ChunkSplitter);

public:
    ChunkSplitter();
    ~ChunkSplitter();

    /**
     * Sets the mode of the ChunkSplitter to either primary or secondary.
     * The ChunkSplitter is only active when primary.
     */
    void setReplicaSetMode(bool isPrimary);

    /**
     * Invoked when the shard server primary enters the 'PRIMARY' state to set up the ChunkSplitter
     * to begin accepting split requests.
     */
    void initiateChunkSplitter();

    /**
     * Invoked when this node which is currently serving as a 'PRIMARY' steps down.
     *
     * This method might be called multiple times in succession, which is what happens as a result
     * of incomplete transition to primary so it is resilient to that.
     */
    void interruptChunkSplitter();

    /**
     * Schedules an autosplit task. This function throws on scheduling failure.
     */
    void trySplitting(const NamespaceString& nss, const BSONObj& min, const BSONObj& max);

private:
    /**
     * Determines if the specified chunk should be split and then performs any necessary split.
     */
    void _runAutosplit(const NamespaceString& nss, const BSONObj& min, const BSONObj& max);

    // Protects the state below.
    stdx::mutex _mutex;

    // The ChunkSplitter is only active on a primary node.
    bool _isPrimary;

    // Thread pool for parallelizing splits.
    ThreadPool _threadPool;
};

}  // namespace mongo
