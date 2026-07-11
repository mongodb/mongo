// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This class wraps the different kinds of command requests into a generically usable write command
 * request that can be passed around.
 */
class BatchedCommandRequest {
public:
    enum BatchType { BatchType_Insert, BatchType_Update, BatchType_Delete };

    BatchedCommandRequest(write_ops::InsertCommandRequest insertOp)
        : _batchType(BatchType_Insert),
          _insertReq(std::make_unique<write_ops::InsertCommandRequest>(std::move(insertOp))) {}

    BatchedCommandRequest(std::unique_ptr<write_ops::InsertCommandRequest> insertOp)
        : _batchType(BatchType_Insert), _insertReq(std::move(insertOp)) {}

    BatchedCommandRequest(write_ops::UpdateCommandRequest updateOp)
        : _batchType(BatchType_Update),
          _updateReq(std::make_unique<write_ops::UpdateCommandRequest>(std::move(updateOp))) {}

    BatchedCommandRequest(std::unique_ptr<write_ops::UpdateCommandRequest> updateOp)
        : _batchType(BatchType_Update), _updateReq(std::move(updateOp)) {}

    BatchedCommandRequest(write_ops::DeleteCommandRequest deleteOp)
        : _batchType(BatchType_Delete),
          _deleteReq(std::make_unique<write_ops::DeleteCommandRequest>(std::move(deleteOp))) {}

    BatchedCommandRequest(std::unique_ptr<write_ops::DeleteCommandRequest> deleteOp)
        : _batchType(BatchType_Delete), _deleteReq(std::move(deleteOp)) {}

    BatchedCommandRequest(BatchedCommandRequest&&) = default;
    BatchedCommandRequest& operator=(BatchedCommandRequest&&) = default;

    static BatchedCommandRequest parseInsert(const OpMsgRequest& request);
    static BatchedCommandRequest parseUpdate(const OpMsgRequest& request);
    static BatchedCommandRequest parseDelete(const OpMsgRequest& request);

    BatchType getBatchType() const {
        return _batchType;
    }

    const NamespaceString& getNS() const;

    const boost::optional<UUID>& getCollectionUUID() const;

    bool getOrdered() const;

    bool getBypassDocumentValidation() const;

    bool hasEncryptionInformation() const;

    const auto& getInsertRequest() const {
        invariant(_insertReq);
        return *_insertReq;
    }

    const auto& getUpdateRequest() const {
        invariant(_updateReq);
        return *_updateReq;
    }

    const auto& getDeleteRequest() const {
        invariant(_deleteReq);
        return *_deleteReq;
    }

    std::unique_ptr<write_ops::InsertCommandRequest> extractInsertRequest() {
        return std::move(_insertReq);
    }

    std::unique_ptr<write_ops::UpdateCommandRequest> extractUpdateRequest() {
        return std::move(_updateReq);
    }

    std::unique_ptr<write_ops::DeleteCommandRequest> extractDeleteRequest() {
        return std::move(_deleteReq);
    }

    std::size_t sizeWriteOps() const;

    void setShardVersion(ShardVersion shardVersion);

    bool hasShardVersion() const;

    const ShardVersion& getShardVersion() const;

    void setDbVersion(DatabaseVersion dbVersion);

    bool hasDbVersion() const;

    const DatabaseVersion& getDbVersion() const;

    GenericArguments& getGenericArguments();
    const GenericArguments& getGenericArguments() const;

    void setLegacyRuntimeConstants(LegacyRuntimeConstants runtimeConstants);

    void unsetLegacyRuntimeConstants();

    bool hasLegacyRuntimeConstants() const;

    const boost::optional<LegacyRuntimeConstants>& getLegacyRuntimeConstants() const;
    const boost::optional<BSONObj>& getLet() const;
    void setLet(boost::optional<BSONObj> value);
    const OptionalBool& getBypassEmptyTsReplacement() const;

    /**
     * Utility which handles evaluating and storing any let parameters based on the request type.
     */
    void evaluateAndReplaceLetParams(OperationContext* opCtx);

    write_ops::WriteCommandRequestBase& getWriteCommandRequestBase();
    const write_ops::WriteCommandRequestBase& getWriteCommandRequestBase() const;
    void setWriteCommandRequestBase(write_ops::WriteCommandRequestBase writeCommandBase);

    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;
    std::string toString() const;

    /**
     * Gets an estimate of the size, in bytes, of the top-level fields in the command.
     */
    int getBaseCommandSizeEstimate(OperationContext* opCtx) const;

    /**
     * Generates a new request, the same as the old, but with insert _ids if required.
     */
    static BatchedCommandRequest cloneInsertWithIds(BatchedCommandRequest origCmdRequest);

    /**
     * Returns batch of delete operations to be attached to a transaction
     */
    static BatchedCommandRequest buildDeleteOp(const NamespaceString& nss,
                                               const BSONObj& query,
                                               bool multiDelete,
                                               const boost::optional<BSONObj>& hint = boost::none);

    /**
     * Returns batch of insert operations to be attached to a transaction.
     */
    static BatchedCommandRequest buildInsertOp(const NamespaceString& nss,
                                               std::vector<BSONObj> docs);

    /**
     * Returns batch of update operations to be attached to a transaction.
     */
    static BatchedCommandRequest buildUpdateOp(const NamespaceString& nss,
                                               const BSONObj& query,
                                               const BSONObj& update,
                                               bool upsert,
                                               bool multi,
                                               const boost::optional<BSONObj>& hint = boost::none);

    /**
     * Returns batch of pipeline update operations to be attached to a transaction.
     */
    static BatchedCommandRequest buildPipelineUpdateOp(const NamespaceString& nss,
                                                       const BSONObj& query,
                                                       const std::vector<BSONObj>& updates,
                                                       bool upsert,
                                                       bool useMultiUpdate);

    /**
     * These are used to return empty refs from Insert ops that don't carry runtimeConstants
     * or let parameters in getLet and getLegacyRuntimeConstants.
     */
    const static boost::optional<LegacyRuntimeConstants> kEmptyRuntimeConstants;
    const static boost::optional<BSONObj> kEmptyLet;

private:
    template <typename Req, typename F, typename... As>
    static decltype(auto) _visitImpl(Req&& r, F&& f, As&&... as) {
        switch (r._batchType) {
            case BatchedCommandRequest::BatchType_Insert:
                return std::forward<F>(f)(*r._insertReq, std::forward<As>(as)...);
            case BatchedCommandRequest::BatchType_Update:
                return std::forward<F>(f)(*r._updateReq, std::forward<As>(as)...);
            case BatchedCommandRequest::BatchType_Delete:
                return std::forward<F>(f)(*r._deleteReq, std::forward<As>(as)...);
        }
        MONGO_UNREACHABLE;
    }
    template <typename... As>
    decltype(auto) _visit(As&&... as) {
        return _visitImpl(*this, std::forward<As>(as)...);
    }
    template <typename... As>
    decltype(auto) _visit(As&&... as) const {
        return _visitImpl(*this, std::forward<As>(as)...);
    }

    BatchType _batchType;

    std::unique_ptr<write_ops::InsertCommandRequest> _insertReq;
    std::unique_ptr<write_ops::UpdateCommandRequest> _updateReq;
    std::unique_ptr<write_ops::DeleteCommandRequest> _deleteReq;
};

BatchedCommandRequest::BatchType convertOpType(BulkWriteCRUDOp::OpType opType);

/**
 * Validates that the 'isTimeseriesNamespace' internal parameter is not set. This parameter is
 * used for communication between mongos and mongod and should not be set by external clients.
 */
void checkIsTimeseriesNamespace(const write_ops::WriteCommandRequestBase& wcb);

}  // namespace mongo
