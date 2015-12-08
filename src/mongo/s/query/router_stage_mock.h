/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <queue>

#include "mongo/s/query/router_exec_stage.h"

namespace mongo {

/**
 * Initialized by adding results to its results queue, it then passes through the results in its
 * queue until the queue is empty.
 */
class RouterStageMock final : public RouterExecStage {
public:
    ~RouterStageMock() final {}

    StatusWith<boost::optional<BSONObj>> next() final;

    void kill() final;

    bool remotesExhausted() final;

    Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) final;

    /**
     * Queues a BSONObj to be returned.
     */
    void queueResult(BSONObj obj);

    /**
     * Queues an error response.
     */
    void queueError(Status status);

    /**
     * Queues an explicit boost::none response. The mock stage will also return boost::none
     * automatically after emptying the queue of responses.
     */
    void queueEOF();

    /**
     * Explicitly marks the remote cursors as all exhausted.
     */
    void markRemotesExhausted();

    /**
     * Gets the timeout for awaitData, or an error if none was set.
     */
    StatusWith<Milliseconds> getAwaitDataTimeout();

private:
    std::queue<StatusWith<boost::optional<BSONObj>>> _resultsQueue;
    bool _remotesExhausted = false;
    boost::optional<Milliseconds> _awaitDataTimeout;
};

}  // namespace mongo
