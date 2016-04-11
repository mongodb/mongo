/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <string>
#include <vector>

#include "mongo/util/background.h"
#include "mongo/util/timer.h"

namespace mongo {

class ClusterStatistics;
struct MigrateInfo;
class MigrationSecondaryThrottleOptions;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * The balancer is a background task that tries to keep the number of chunks across all
 * servers of the cluster even. Although every mongos will have one balancer running, only one
 * of them will be active at the any given point in time. The balancer uses a distributed lock
 * for that coordination.
 *
 * The balancer does act continuously but in "rounds". At a given round, it would decide if
 * there is an imbalance by checking the difference in chunks between the most and least
 * loaded shards. It would issue a request for a chunk migration per round, if it found so.
 */
class Balancer : public BackgroundJob {
public:
    Balancer();
    ~Balancer();

    /**
     * Retrieves the per-service instance of the Balancer.
     */
    static Balancer* get(OperationContext* operationContext);

    ClusterStatistics* getClusterStatistics() const {
        return _clusterStats.get();
    }

    // BackgroundJob methods

    virtual void run();

    virtual std::string name() const {
        return "Balancer";
    }

private:
    /**
     * Checks that the balancer can connect to all servers it needs to do its job.
     *
     * @return true if balancing can be started
     *
     * This method throws on a network exception
     */
    bool _init(OperationContext* txn);

    /**
     * Marks this balancer as being live on the config server(s).
     */
    void _ping(OperationContext* txn, bool waiting);

    /**
     * Returns true if all the servers listed in configdb as being shards are reachable and are
     * distinct processes (no hostname mixup).
     */
    bool _checkOIDs(OperationContext* txn);

    /**
     * Gathers all the necessary information about shards and chunks, and decides whether there are
     * candidate chunks to be moved.
     *
     * Returns candidate chunks, one per collection, that could possibly be moved
     */
    StatusWith<std::vector<MigrateInfo>> _getCandidateChunks(OperationContext* txn);

    /**
     * Issues chunk migration request, one at a time.
     *
     * @param candidateChunks possible chunks to move
     * @param writeConcern detailed write concern. NULL means the default write concern.
     * @param waitForDelete wait for deletes to complete after each chunk move
     * @return number of chunks effectively moved
     */
    int _moveChunks(OperationContext* txn,
                    const std::vector<MigrateInfo>& candidateChunks,
                    const MigrationSecondaryThrottleOptions& secondaryThrottle,
                    bool waitForDelete);

    // hostname:port of my mongos
    std::string _myid;

    // Time the Balancer started running
    Timer _timer;

    // number of moved chunks in last round
    int _balancedLastTime;

    // Source for cluster statistics
    std::unique_ptr<ClusterStatistics> _clusterStats;
};

}  // namespace mongo
