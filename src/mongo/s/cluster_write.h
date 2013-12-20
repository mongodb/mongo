/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

    class ClusterWriterStats;
    class BatchWriteExecStats;

    class ClusterWriter {
    public:

        ClusterWriter( bool autoSplit, int timeoutMillis );

        void write( const BatchedCommandRequest& request, BatchedCommandResponse* response );

        const ClusterWriterStats& getStats();

    private:

        void configWrite( const BatchedCommandRequest& request,
                          BatchedCommandResponse* response,
                          bool fsyncCheck );

        void shardWrite( const BatchedCommandRequest& request,
                         BatchedCommandResponse* response );

        bool _autoSplit;
        int _timeoutMillis;

        scoped_ptr<ClusterWriterStats> _stats;
    };

    class ClusterWriterStats {
    public:

        // Transfers ownership to the cluster write stats
        void setShardStats( BatchWriteExecStats* _shardStats );

        bool hasShardStats() const;

        const BatchWriteExecStats& getShardStats() const;

        // TODO: When we have ConfigCoordinator stats, put these here too.

    private:

        scoped_ptr<BatchWriteExecStats> _shardStats;
    };

    const BSONObj DefaultClusterWriteConcern = BSONObj();

    void clusterWrite( const BatchedCommandRequest& request,
                       BatchedCommandResponse* response,
                       bool autoSplit );

    void clusterInsert( const std::string& ns,
                        const BSONObj& doc,
                        const BSONObj& writeConcern,
                        BatchedCommandResponse* response );

    void clusterUpdate( const std::string& ns,
                        const BSONObj& query,
                        const BSONObj& update,
                        bool upsert,
                        bool multi,
                        const BSONObj& writeConcern,
                        BatchedCommandResponse* response );

    void clusterDelete( const std::string& ns,
                        const BSONObj& query,
                        int limit,
                        const BSONObj& writeConcern,
                        BatchedCommandResponse* response );

    void clusterCreateIndex( const std::string& ns,
                             BSONObj keys,
                             bool unique,
                             const BSONObj& writeConcern,
                             BatchedCommandResponse* response );

} // namespace mongo
