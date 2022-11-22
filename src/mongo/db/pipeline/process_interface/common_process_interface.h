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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"

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
     * does not account for the full size of an update statement.
     */
    class LocalWriteSizeEstimator final : public WriteSizeEstimator {
    public:
        int estimateInsertSizeBytes(const BSONObj& insert) const override {
            return insert.objsize();
        }

        int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                    UpsertType type) const override {
            int size = std::get<write_ops::UpdateModification>(batchObject).objsize();
            if (auto vars = std::get<boost::optional<BSONObj>>(batchObject)) {
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
        int estimateInsertSizeBytes(const BSONObj& insert) const override {
            return insert.objsize() + write_ops::kWriteCommandBSONArrayPerElementOverheadBytes;
        }

        int estimateUpdateSizeBytes(const BatchObject& batchObject,
                                    UpsertType type) const override {
            return getUpdateSizeEstimate(std::get<BSONObj>(batchObject),
                                         std::get<write_ops::UpdateModification>(batchObject),
                                         std::get<boost::optional<BSONObj>>(batchObject),
                                         type != UpsertType::kNone /* includeUpsertSupplied */,
                                         boost::none /* collation */,
                                         boost::none /* arrayFilters */,
                                         BSONObj() /* hint*/,
                                         boost::none /* sampleId */) +
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

    std::vector<BSONObj> getCurrentOps(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       CurrentOpConnectionsMode connMode,
                                       CurrentOpSessionsMode sessionMode,
                                       CurrentOpUserMode userMode,
                                       CurrentOpTruncateMode truncateMode,
                                       CurrentOpCursorMode cursorMode,
                                       CurrentOpBacktraceMode backtraceMode) const final;

    virtual std::vector<FieldPath> collectDocumentKeyFieldsActingAsRouter(
        OperationContext*, const NamespaceString&) const override;

    std::unique_ptr<WriteSizeEstimator> getWriteSizeEstimator(
        OperationContext* opCtx, const NamespaceString& ns) const override;

    virtual void updateClientOperationTime(OperationContext* opCtx) const final;

    boost::optional<ShardVersion> refreshAndGetCollectionVersion(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss) const override;

    std::string getHostAndPort(OperationContext* opCtx) const override;

protected:
    /**
     * Converts the fields from a ShardKeyPattern to a vector of FieldPaths, including the _id if
     * it's not already in 'keyPatternFields'.
     */
    std::vector<FieldPath> _shardKeyToDocumentKeyFields(
        const std::vector<std::unique_ptr<FieldRef>>& keyPatternFields) const;

    /**
     * Returns a BSONObj representing a report of the operation which is currently being
     * executed by the supplied client. This method is called by the getCurrentOps method of
     * CommonProcessInterface to delegate to the mongoS- or mongoD- specific implementation.
     */
    virtual BSONObj _reportCurrentOpForClient(OperationContext* opCtx,
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
};

}  // namespace mongo
