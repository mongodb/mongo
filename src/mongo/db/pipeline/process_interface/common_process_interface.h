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

#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <boost/exception/exception.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * CommonProcessInterface provides base implementations of any MongoProcessInterface methods
 * whose code is largely identical on mongoD and mongoS.
 */
class CommonProcessInterface : public MongoProcessInterface {
public:
    using MongoProcessInterface::MongoProcessInterface;
    virtual ~CommonProcessInterface() = default;

    /**
     * Estimates the size of writes that will be executed on the current node. Note that this
     * does not account for the full size of an update statement because in the case of local
     * writes, we will not have to serialize to BSON and are therefore not subject to the 16MB
     * BSONObj size limit.
     */
    class LocalWriteSizeEstimator final : public WriteSizeEstimator {
    public:
        int estimateInsertHeaderSize(
            const write_ops::InsertCommandRequest& insertReq) const override {
            return 0;
        }

        int estimateUpdateHeaderSize(
            const write_ops::UpdateCommandRequest& insertReq) const override {
            return 0;
        }

        int estimateInsertSizeBytes(const BSONObj& insert) const override {
            return insert.objsize();
        }

        int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                    UpsertType type) const override {
            int size = get<write_ops::UpdateModification>(batchObject).objsize();
            if (auto vars = get<boost::optional<BSONObj>>(batchObject)) {
                size += vars->objsize();
            }
            return size;
        }
    };

    /**
     * Estimate the size of writes that will be sent to the replica set primary.
     */
    class TargetPrimaryWriteSizeEstimator final : public WriteSizeEstimator {
    public:
        int estimateInsertHeaderSize(
            const write_ops::InsertCommandRequest& insertReq) const override {
            return write_ops::getInsertHeaderSizeEstimate(insertReq);
        }

        int estimateUpdateHeaderSize(
            const write_ops::UpdateCommandRequest& updateReq) const override {
            return write_ops::getUpdateHeaderSizeEstimate(updateReq);
        }

        int estimateInsertSizeBytes(const BSONObj& insert) const override {
            return insert.objsize() + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
        }

        int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                    UpsertType type) const override {
            return getUpdateSizeEstimate(
                       get<BSONObj>(batchObject),
                       get<write_ops::UpdateModification>(batchObject),
                       get<boost::optional<BSONObj>>(batchObject),
                       type != UpsertType::kNone /* includeUpsertSupplied */,
                       boost::none /* collation */,
                       boost::none /* arrayFilters */,
                       boost::none /* sort */,
                       BSONObj() /* hint*/,
                       boost::none /* sampleId */,
                       false /* $_allowShardKeyUpdatesWithoutFullShardKeyInQuery */) +
                write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
        }
    };

    /**
     * Returns true if the field names of 'keyPattern' are exactly those in 'uniqueKeyPaths', and
     * each of the elements of 'keyPattern' is numeric, i.e. not "text", "$**", or any other special
     * type of index.
     */
    static bool keyPatternNamesExactPaths(const BSONObj& keyPattern,
                                          const std::set<FieldPath>& uniqueKeyPaths);

    /**
     * Converts the fields from a ShardKeyPattern to a vector of FieldPaths, including the _id if
     * it's not already in 'keyPatternFields'.
     */
    static std::vector<FieldPath> shardKeyToDocumentKeyFields(
        const std::vector<std::unique_ptr<FieldRef>>& keyPatternFields);

    /**
     * Utility which determines which shard owns 'nss'. More precisely, if 'nss' resides on
     * a single shard and is not sharded (that is, it is either unsplittable or untracked), we
     * return the id of the shard which owns 'nss'. Note that this decision is inherently racy and
     * subject to become stale. This is okay because either choice will work correctly, we are
     * simply applying a heuristic optimization.
     *
     * As written, this function can only be called in a sharded context.
     *
     * Note that the first overload looks up an instance of 'CatalogCache', while the second takes
     * it as a parameter.
     */
    static boost::optional<ShardId> findOwningShard(OperationContext* opCtx,
                                                    const NamespaceString& nss);
    static boost::optional<ShardId> findOwningShard(OperationContext* opCtx,
                                                    CatalogCache* catalogCache,
                                                    const NamespaceString& nss);


    std::vector<BSONObj> getCurrentOps(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode,
                                       CurrentOpCursorMode cursorMode,
                                       CurrentOpBacktraceMode backtraceMode) const final;

    virtual std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const override;

    virtual void updateClientOperationTime(OperationContext* opCtx) const final;

    boost::optional<ShardVersion> refreshAndGetCollectionVersion(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss) const override;

    boost::optional<ShardId> determineSpecificMergeShard(
        OperationContext* opCtx, const NamespaceString& nss) const override {
        return boost::none;
    };

    std::string getHostAndPort(OperationContext* opCtx) const override;

protected:
    /**
     * Returns a BSONObj representing a report of the operation which is currently being
     * executed by the supplied client. This method is called by the getCurrentOps method of
     * CommonProcessInterface to delegate to the mongoS- or mongoD- specific implementation.
     */
    virtual BSONObj _reportCurrentOpForClient(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              Client* client,
                                              CurrentOpTruncateMode truncateOps,
                                              CurrentOpBacktraceMode backtraceMode) const = 0;

    /**
     * Iterates through all entries in the local SessionCatalog, and adds an entry to the 'ops'
     * vector for each idle session that has stashed its transaction locks while sleeping.
     */
    virtual void _reportCurrentOpsForIdleSessions(OperationContext* opCtx,
                                                  CurrentOpUserMode userMode,
                                                  std::vector<BSONObj>* ops) const = 0;


    /**
     * Report information about transaction coordinators by iterating through all
     * TransactionCoordinators in the TransactionCoordinatorCatalog.
     */
    virtual void _reportCurrentOpsForTransactionCoordinators(OperationContext* opCtx,
                                                             bool includeIdle,
                                                             std::vector<BSONObj>* ops) const = 0;

    /**
     * Reports information about PrimaryOnlyServices.
     */
    virtual void _reportCurrentOpsForPrimaryOnlyServices(OperationContext* opCtx,
                                                         CurrentOpConnectionsMode connMode,
                                                         CurrentOpSessionsMode sessionMode,
                                                         std::vector<BSONObj>* ops) const = 0;

    /**
     * Reports information about query sampling.
     */
    virtual void _reportCurrentOpsForQueryAnalysis(OperationContext* opCtx,
                                                   std::vector<BSONObj>* ops) const = 0;
};

}  // namespace mongo
