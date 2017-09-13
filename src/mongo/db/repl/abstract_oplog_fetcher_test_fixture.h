/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

/**
 * This class represents the state at shutdown of an abstract oplog fetcher.
 */
class ShutdownState {
    MONGO_DISALLOW_COPYING(ShutdownState);

public:
    ShutdownState();

    /**
     * Returns the status at shutdown.
     */
    Status getStatus() const;

    /**
     * Use this for oplog fetcher shutdown callback.
     */
    void operator()(const Status& status);

private:
    Status _status = executor::TaskExecutorTest::getDetectableErrorStatus();
};

/**
 * This class contains many of the functions used by all oplog fetcher test suites.
 */
class AbstractOplogFetcherTest : public executor::ThreadPoolExecutorTest {
public:
    /**
     * Static functions for creating noop oplog entries.
     */
    static BSONObj makeNoopOplogEntry(OpTimeWithHash opTimeWithHash);
    static BSONObj makeNoopOplogEntry(OpTime opTime, long long hash);
    static BSONObj makeNoopOplogEntry(Seconds seconds, long long hash);

    /**
     * A static function for creating the response to a cursor. If it's the last batch, the
     * cursorId provided should be 0.
     */
    static BSONObj makeCursorResponse(
        CursorId cursorId,
        Fetcher::Documents oplogEntries,
        bool isFirstBatch = true,
        const NamespaceString& nss = NamespaceString("local.oplog.rs"));

protected:
    void setUp() override;

    /**
     * Schedules network response and instructs network interface to process response.
     * Returns remote command request in network request.
     */
    executor::RemoteCommandRequest processNetworkResponse(
        executor::RemoteCommandResponse response, bool expectReadyRequestsAfterProcessing = false);
    executor::RemoteCommandRequest processNetworkResponse(
        BSONObj obj, bool expectReadyRequestsAfterProcessing = false);

    // The last OpTime and hash fetched by the oplog fetcher.
    OpTimeWithHash lastFetched;
};
}  // namespace repl
}  // namespace mango
