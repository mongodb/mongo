/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/client/sdam/topology_listener.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"

namespace mongo {
namespace serverless {
/**
 * @returns A list of `MemberConfig` for member nodes which match a provided replica set tag name
 */
std::vector<repl::MemberConfig> getRecipientMembers(const repl::ReplSetConfig& config,
                                                    const StringData& recipientTagName);


/**
 * Builds a connection string for a shard split recipient by filtering local member nodes by
 * `recipientTagName`. The `recipientSetName` is the `replSet` parameter of the recipient
 * connection string.
 */
ConnectionString makeRecipientConnectionString(const repl::ReplSetConfig& config,
                                               const StringData& recipientTagName,
                                               const StringData& recipientSetName);

/**
 * Builds a split config, which is a ReplSetConfig with a subdocument identifying a recipient config
 * to be applied to a recipient shard during a shard split operation. The `recipientTagName` is used
 * to filter the local member list for recipient nodes. The `recipientSetName` is used to validate
 * that we are indeed generating a config for a recipient set with a new name.
 */
repl::ReplSetConfig makeSplitConfig(const repl::ReplSetConfig& config,
                                    const std::string& recipientSetName,
                                    const std::string& recipientTagName,
                                    const repl::OpTime& blockOpTime);

/**
 * Inserts the shard split state document 'stateDoc' into
 * 'config.shardSplitDonors' collection. Also, creates the collection if not present
 * before inserting the document.
 *
 * NOTE: A state doc might get inserted based on a decision made out of a stale read within a
 * storage transaction. Callers are expected to have their own concurrency mechanism to handle
 * write skew problem.
 *
 * @Returns 'ConflictingOperationInProgress' error code if an active shard split op found for the
 * given state doc id provided in the 'stateDoc'.
 *
 * Throws 'DuplicateKey' error code if a document already exists on the disk with the same
 * shardSplitId, irrespective of the document marked for garbage collect or not.
 */
Status insertStateDoc(OperationContext* opCtx, const ShardSplitDonorDocument& stateDoc);

/**
 * Updates the shard split state doc in the database.
 *
 * Returns 'NoSuchKey' error code if no state document already exists on the disk with the same
 * shardSplitId.
 */
Status updateStateDoc(OperationContext* opCtx, const ShardSplitDonorDocument& stateDoc);


/**
 * Deletes a state documents in the database for a recipient if the state is blocking at startup.
 *
 * Returns 'NamespaceNotFound' if no matching namespace is found. Returns true if the doc was
 * removed.
 */
StatusWith<bool> deleteStateDoc(OperationContext* opCtx, const UUID& shardSplitId);

/**
 * Returns true if the state document should be removed for a shard split recipient which is based
 * on having a local state doc in kBlocking state and having matching recipientSetName matching the
 * config.replSetName.
 */
bool shouldRemoveStateDocumentOnRecipient(OperationContext* opCtx,
                                          const ShardSplitDonorDocument& stateDocument);

/**
 * Returns StatusWith true if the validation succeeds otherwise returns different error status with
 * the proper error causing the failure.
 */
Status validateRecipientNodesForShardSplit(const ShardSplitDonorDocument& stateDocument,
                                           const repl::ReplSetConfig& localConfig);
/**
 * Listener that receives heartbeat events and fulfills a future once it sees the expected number
 * of nodes in the recipient replica set to monitor.
 */
class RecipientAcceptSplitListener : public sdam::TopologyListener {
public:
    RecipientAcceptSplitListener(const ConnectionString& recipientConnectionString);

    void onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort, BSONObj reply) final;

    // Fulfilled when all nodes have accepted the split.
    SharedSemiFuture<HostAndPort> getSplitAcceptedFuture() const;

private:
    mutable Mutex _mutex =
        MONGO_MAKE_LATCH("ShardSplitDonorService::getRecipientAcceptSplitFuture::_mutex");

    bool _fulfilled{false};
    const size_t _numberOfRecipient;
    std::string _recipientSetName;
    stdx::unordered_map<HostAndPort, repl::OpTimeWith<std::string>> _reportedSetNames;
    SharedPromise<HostAndPort> _promise;
};

}  // namespace serverless
}  // namespace mongo
