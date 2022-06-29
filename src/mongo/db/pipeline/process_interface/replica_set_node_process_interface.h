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

#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/non_shardsvr_process_interface.h"

namespace mongo {

/**
 * An implementation of the MongoProcessInterface used on replica set nodes when sharding is not
 * enabled.
 */
class ReplicaSetNodeProcessInterface final : public NonShardServerProcessInterface {
public:
    using NonShardServerProcessInterface::NonShardServerProcessInterface;

    static std::shared_ptr<executor::TaskExecutor> getReplicaSetNodeExecutor(
        ServiceContext* service);

    static std::shared_ptr<executor::TaskExecutor> getReplicaSetNodeExecutor(
        OperationContext* opCtx);

    static void setReplicaSetNodeExecutor(ServiceContext* service,
                                          std::shared_ptr<executor::TaskExecutor> executor);

    ReplicaSetNodeProcessInterface(std::shared_ptr<executor::TaskExecutor> executor)
        : NonShardServerProcessInterface(std::move(executor)) {}

    virtual ~ReplicaSetNodeProcessInterface() = default;

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) final;
    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    BatchedObjects&& batch,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) override;

    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const NamespaceString& sourceNs,
                                                 const NamespaceString& targetNs,
                                                 bool dropTarget,
                                                 bool stayTemp,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes);
    void createCollection(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const BSONObj& cmdObj);
    void dropCollection(OperationContext* opCtx, const NamespaceString& collection);
    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs);

private:
    /**
     * Attemps to execute the specified command on the primary. Returns the command response upon
     * success or a non-OK status upon a failed command response, a writeConcernError, or any
     * writeErrors.
     */
    StatusWith<BSONObj> _executeCommandOnPrimary(OperationContext* opCtx,
                                                 const NamespaceString& ns,
                                                 const BSONObj& cmdObj) const;

    /**
     * Attaches command arguments such as writeConcern to 'cmd'.
     */
    void _attachGenericCommandArgs(OperationContext* opCtx, BSONObjBuilder* cmd) const;

    /**
     * Returns whether we are the primary and can therefore perform writes locally. Result may be
     * immediately stale upon return.
     */
    bool _canWriteLocally(OperationContext* opCtx, const NamespaceString& ns) const;
};

}  // namespace mongo
