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

#include <memory>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"


namespace mongo {
namespace repl {
class BackgroundSync;
class ReplicationCoordinator;

/**
 * This class is used to apply
 **/
class RSDataSync {
public:
    RSDataSync(BackgroundSync* bgsync, ReplicationCoordinator* replCoord);
    void startup();
    void shutdown();
    void join();

private:
    // Runs in a loop apply oplog entries from the buffer until this class cancels, or an error.
    void _run();
    bool _isInShutdown() const;

    // _mutex protects all of the class variables declared below.
    mutable stdx::mutex _mutex;
    // Thread doing the work.
    std::unique_ptr<stdx::thread> _runThread;
    // Set to true if shutdown() has been called.
    bool _inShutdown = false;
    // If the thread should not be running.
    bool _stopped = true;
    // BackgroundSync instance that is paired to this instance.
    BackgroundSync* _bgsync;
    // ReplicationCordinator instance.
    ReplicationCoordinator* _replCoord;
};

}  // namespace repl
}  // namespace mongo
