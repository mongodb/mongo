/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/background.h"

namespace mongo {

class Collection;
class Database;
class OperationContext;

/**
 * A helper class for replication to use for building indexes.
 * In standalone mode, we use the client connection thread for building indexes in the
 * background. In replication mode, secondaries must spawn a new thread to build background
 * indexes, since there are no client connection threads to use for such purpose.  IndexBuilder
 * is a subclass of BackgroundJob to enable this use.
 * This class is also used for building indexes in the foreground on secondaries, for
 * code convenience.  buildInForeground() is directly called by the replication applier to
 * build an index in the foreground; the properties of BackgroundJob are not used for this use
 * case.
 * For background index builds, BackgroundJob::go() is called on the IndexBuilder instance,
 * which begins a new thread at this class's run() method.  After go() is called in the
 * parent thread, waitForBgIndexStarting() must be called by the same parent thread,
 * before any other thread calls go() on any other IndexBuilder instance.  This is
 * ensured by the replication system, since commands are effectively run single-threaded
 * by the replication applier, and index builds are treated as commands even though they look
 * like inserts on system.indexes.
 */
class IndexBuilder : public BackgroundJob {
public:
    IndexBuilder(const BSONObj& index);
    virtual ~IndexBuilder();

    virtual void run();

    /**
     * name of the builder, not the index
     */
    virtual std::string name() const;

    Status buildInForeground(OperationContext* txn, Database* db) const;

    /**
     * Waits for a background index build to register itself.  This function must be called
     * after starting a background index build via a BackgroundJob and before starting a
     * subsequent one.
     */
    static void waitForBgIndexStarting();

private:
    Status _build(OperationContext* txn,
                  Database* db,
                  bool allowBackgroundBuilding,
                  Lock::DBLock* dbLock) const;

    const BSONObj _index;
    std::string _name;  // name of this builder, not related to the index
    static AtomicUInt32 _indexBuildCount;
};
}
