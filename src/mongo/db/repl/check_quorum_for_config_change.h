/**
 *    Copyright (C) 2014 MongoDB Inc.
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

namespace mongo {

    class Status;

namespace repl {

    class ReplicaSetConfig;
    class ReplicationExecutor;

    /**
     * Performs a quorum call to determine if a sufficient number of nodes are up
     * to initiate a replica set with configuration "rsConfig".
     *
     * "myIndex" is the index of this node's member configuration in "rsConfig".
     * "executor" is the event loop in which to schedule network/aysnchronous processing.
     *
     * For purposes of initiate, a quorum is only met if all of the following conditions
     * are met:
     * - All nodes respond.
     * - No nodes other than the node running the quorum check have data.
     * - No nodes are already joined to a replica set.
     * - No node reports a replica set name other than the one in "rsConfig".
     */
    Status checkQuorumForInitiate(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex);

    /**
     * Performs a quorum call to determine if a sufficient number of nodes are up
     * to replace the current replica set configuration with "rsConfig".
     *
     * "myIndex" is the index of this node's member configuration in "rsConfig".
     * "executor" is the event loop in which to schedule network/aysnchronous processing.
     *
     * For purposes of reconfig, a quorum is only met if all of the following conditions
     * are met:
     * - A majority of voting nodes respond.
     * - At least one electable node responds.
     * - No responding node reports a replica set name other than the one in "rsConfig".
     * - All responding nodes report a config version less than the one in "rsConfig".
     */
    Status checkQuorumForReconfig(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex);

}  // namespace repl
}  // namespace mongo
