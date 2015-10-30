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

#include "mongo/db/jsobj.h"
#include "mongo/util/time_support.h"

namespace mongo {

template <typename T>
class StatusWith;

/**
 * ClusterClientCursor is used to generate results from cursor-generating commands on one or
 * more remote hosts. A cursor-generating command (e.g. the find command) is one that
 * establishes a ClientCursor and a matching cursor id on the remote host. In order to retrieve
 * all command results, getMores must be issued against each of the remote cursors until they
 * are exhausted.
 *
 * Results are generated using a pipeline of mongoS query execution stages called RouterExecStage.
 *
 * Does not throw exceptions.
 */
class ClusterClientCursor {
public:
    virtual ~ClusterClientCursor(){};

    /**
     * Returns the next available result document (along with an ok status). May block waiting
     * for results from remote nodes.
     *
     * If there are no further results, the end of the stream is indicated with boost::none and
     * an ok status.
     *
     * A non-ok status is returned in case of any error.
     */
    virtual StatusWith<boost::optional<BSONObj>> next() = 0;

    /**
     * Must be called before destruction to abandon a not-yet-exhausted cursor. If next() has
     * already returned boost::none, then the cursor is exhausted and is safe to destroy.
     *
     * May block waiting for responses from remote hosts.
     */
    virtual void kill() = 0;

    /**
     * Returns whether or not this cursor is tailing a capped collection on a shard.
     */
    virtual bool isTailable() const = 0;

    /**
     * Returns the number of result documents returned so far by this cursor via the next() method.
     */
    virtual long long getNumReturnedSoFar() const = 0;

    /**
     * Stash the BSONObj so that it gets returned from the CCC on a later call to next().
     *
     * Queued documents are returned in FIFO order. The queued results are exhausted before
     * generating further results from the underlying mongos query stages.
     *
     * 'obj' must be owned BSON.
     */
    virtual void queueResult(const BSONObj& obj) = 0;

    /**
     * Returns whether or not all the remote cursors underlying this cursor have been exhausted.
     */
    virtual bool remotesExhausted() = 0;

    /**
     * Sets the maxTimeMS value that the cursor should forward with any internally issued getMore
     * requests.
     *
     * Returns a non-OK status if this cursor type does not support maxTimeMS on getMore (i.e. if
     * the cursor is not tailable + awaitData).
     */
    virtual Status setAwaitDataTimeout(Milliseconds awaitDataTimeout) = 0;
};

}  // namespace mongo
