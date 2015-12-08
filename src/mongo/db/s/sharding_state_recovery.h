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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

namespace mongo {

class OperationContext;
class Status;

/**
 * Manages the persistence and recovery of the sharding config metadata's min opTime.
 *
 * The opTime recovery document resides in the admin.system.version collection and has the
 * following format:
 *
 * { _id: "minOpTimeRecovery",
 *   configsvrConnectionString: "config/server1:10000,server2:10001,server3:10002",
 *   shardName: "shard0000",
 *   minOpTime: { ts: Timestamp 1443820968000|1, t: 11 },
 *   minOptimeUpdaters: 1 }
 */
class ShardingStateRecovery {
public:
    /**
     * Marks the beginning of a sharding metadata operation which requires recovery of the config
     * server's minOpTime after node failure. It is only safe to commence the operation after this
     * method returns an OK status.
     */
    static Status startMetadataOp(OperationContext* txn);

    /**
     * Marks the end of a sharding metadata operation, persisting the latest config server opTime at
     * the time of the call.
     */
    static void endMetadataOp(OperationContext* txn);

    /**
     * Recovers the minimal config server opTime that the instance should be using for reading
     * sharding metadata so that the instance observes all metadata modifications it did the last
     * time it was active (or PRIMARY, if replica set).
     *
     * NOTE: This method will block until recovery completes.
     *
     * Returns OK if the minOpTime was successfully recovered or failure status otherwise. It is
     * unsafe to read and rely on any sharding metadata before this method has returned success.
     */
    static Status recover(OperationContext* txn);
};

}  // namespace mongo
