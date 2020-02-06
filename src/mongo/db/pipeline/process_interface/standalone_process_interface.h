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
 * Class to provide access to standalone specific implementations of methods required by some
 * document sources.
 */
class StandaloneProcessInterface : public NonShardServerProcessInterface {
public:
    using NonShardServerProcessInterface::NonShardServerProcessInterface;

    virtual ~StandaloneProcessInterface() = default;

    Status insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                  const NamespaceString& ns,
                  std::vector<BSONObj>&& objs,
                  const WriteConcernOptions& wc,
                  boost::optional<OID> targetEpoch) override;
    StatusWith<UpdateResult> update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const NamespaceString& ns,
                                    BatchedObjects&& batch,
                                    const WriteConcernOptions& wc,
                                    UpsertType upsert,
                                    bool multi,
                                    boost::optional<OID> targetEpoch) override;
    std::list<BSONObj> getIndexSpecs(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     bool includeBuildUUIDs);
    void renameIfOptionsAndIndexesHaveNotChanged(OperationContext* opCtx,
                                                 const BSONObj& renameCommandObj,
                                                 const NamespaceString& targetNs,
                                                 const BSONObj& originalCollectionOptions,
                                                 const std::list<BSONObj>& originalIndexes);
    void createCollection(OperationContext* opCtx,
                          const std::string& dbName,
                          const BSONObj& cmdObj);
    void dropCollection(OperationContext* opCtx, const NamespaceString& collection);
    void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const std::vector<BSONObj>& indexSpecs);
    std::unique_ptr<Pipeline, PipelineDeleter> attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        Pipeline* pipeline,
        bool allowTargetingShards) override;
};

}  // namespace mongo
