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
                                    const std::string& recipientTagName);

/**
 * Inserts the shard split state document 'stateDoc' into
 * 'config.tenantSplitDonors' collection. Also, creates the collection if not present
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
 * Returns the state doc matching the document with shardSplitId from the disk if it
 * exists. Reads at "no" timestamp i.e, reading with the "latest" snapshot reflecting up to date
 * data.
 *
 * If the stored state doc on disk contains invalid BSON, the 'InvalidBSON' error code is
 * returned.
 *
 * Returns 'NoMatchingDocument' error code if no document with 'shardSplitId' is found.
 */
StatusWith<ShardSplitDonorDocument> getStateDocument(OperationContext* opCtx,
                                                     const UUID& shardSplitId);

}  // namespace serverless
}  // namespace mongo
