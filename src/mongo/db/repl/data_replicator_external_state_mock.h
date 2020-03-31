/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/repl/data_replicator_external_state.h"

namespace mongo {
namespace repl {

class ReplicationCoordinator;

/**
 * Data replicator external state implementation for testing.
 */

class DataReplicatorExternalStateMock : public DataReplicatorExternalState {
public:
    DataReplicatorExternalStateMock();

    executor::TaskExecutor* getTaskExecutor() const override;

    OpTimeWithTerm getCurrentTermAndLastCommittedOpTime() override;

    void processMetadata(const rpc::ReplSetMetadata& metadata,
                         rpc::OplogQueryMetadata oqMetadata) override;

    bool shouldStopFetching(const HostAndPort& source,
                            const rpc::ReplSetMetadata& replMetadata,
                            const rpc::OplogQueryMetadata& oqMetadata,
                            const OpTime& lastOpTimeFetched) override;

    std::unique_ptr<OplogBuffer> makeInitialSyncOplogBuffer(OperationContext* opCtx) const override;

    std::unique_ptr<OplogApplier> makeOplogApplier(
        OplogBuffer* oplogBuffer,
        OplogApplier::Observer* observer,
        ReplicationConsistencyMarkers* consistencyMarkers,
        StorageInterface* storageInterface,
        const OplogApplier::Options& options,
        ThreadPool* writerPool) final;

    StatusWith<ReplSetConfig> getCurrentConfig() const override;

    // Task executor. Not owned by us.
    executor::TaskExecutor* taskExecutor = nullptr;

    // Returned by getCurrentTermAndLastCommittedOpTime.
    long long currentTerm = OpTime::kUninitializedTerm;
    OpTime lastCommittedOpTime;

    // Set by processMetadata.
    rpc::ReplSetMetadata replMetadataProcessed;
    rpc::OplogQueryMetadata oqMetadataProcessed;
    bool metadataWasProcessed = false;

    // Set by shouldStopFetching.
    HostAndPort lastSyncSourceChecked;
    OpTime syncSourceLastOpTime;
    bool syncSourceHasSyncSource = false;

    // Returned by shouldStopFetching.
    bool shouldStopFetchingResult = false;

    // Override to change applyOplogBatch behavior.
    using ApplyOplogBatchFn = std::function<StatusWith<OpTime>(
        OperationContext*, std::vector<OplogEntry>, OplogApplier::Observer*)>;
    ApplyOplogBatchFn applyOplogBatchFn;

    StatusWith<ReplSetConfig> replSetConfigResult = ReplSetConfig();
};


}  // namespace repl
}  // namespace mongo
