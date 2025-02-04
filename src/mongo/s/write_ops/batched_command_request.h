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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/query_cmd/bulk_write_crud_op.h"
#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

namespace mongo {

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
    void setLet(boost::optional<mongo::BSONObj> value);
    const OptionalBool& getBypassEmptyTsReplacement() const;

    /**
     * Utility which handles evaluating and storing any let parameters based on the request type.
     */
    void evaluateAndReplaceLetParams(OperationContext* opCtx);

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
     * Returns batch of insert operations to be attached to a transaction
     */
    static BatchedCommandRequest buildInsertOp(const NamespaceString& nss,
                                               std::vector<BSONObj> docs);

    /*
     * Returns batch of update operations to be attached to a transaction
     */
    static BatchedCommandRequest buildUpdateOp(const NamespaceString& nss,
                                               const BSONObj& query,
                                               const BSONObj& update,
                                               bool upsert,
                                               bool multi,
                                               const boost::optional<BSONObj>& hint = boost::none);

    /**
     *  Returns batch of pipeline update operations to be attached to a transaction
     */
    static BatchedCommandRequest buildPipelineUpdateOp(const NamespaceString& nss,
                                                       const BSONObj& query,
                                                       const std::vector<BSONObj>& updates,
                                                       bool upsert,
                                                       bool useMultiUpdate);

    /** These are used to return empty refs from Insert ops that don't carry runtimeConstants
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


/**
 * Provides access to information for an update operation. Used to abstract over whether a
 * BatchItemRef is pointing to a `mongo::write_ops::UpdateOpEntry` (if it's from an `update`
 * command) or a `mongo::BulkWriteUpdateOp` (if it's from a `bulkWrite` command).
 */
class UpdateRef {
public:
    UpdateRef() = delete;
    UpdateRef(const mongo::write_ops::UpdateOpEntry& batchUpdateReq)
        : _batchUpdateRequest(batchUpdateReq) {}
    UpdateRef(const mongo::BulkWriteUpdateOp& bulkUpdateReq)
        : _bulkWriteUpdateRequest(bulkUpdateReq) {}

    /**
     * Returns the filter for the update operation this `UpdateRef` refers to, i.e.  the `q`
     * value for an update operation from an `update` command, or the `filter` value for an update
     * operation from a `bulkWrite` command.
     */
    const BSONObj& getFilter() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getQ();
        } else {
            tassert(7328100, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getFilter();
        }
    }

    /**
     * Returns the arrayFilters the update operation this `UpdateRef` refers to.
     */
    const boost::optional<std::vector<mongo::BSONObj>>& getArrayFilters() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getArrayFilters();
        } else {
            tassert(7961100, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getArrayFilters();
        }
    }

    /**
     * Returns the `multi` value for the update operation this `UpdateRef` refers to.
     */
    bool getMulti() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getMulti();
        } else {
            tassert(7328101, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getMulti();
        }
    }

    /**
     * Returns the `upsert` value for the update operation this `UpdateRef` refers to.
     */
    bool getUpsert() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getUpsert();
        } else {
            tassert(7328102, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getUpsert();
        }
    }

    /**
     * Returns the update modification for the update operation this `UpdateRef` refers to,
     * i.e.  the `u` value for an update operation from an `update` command, or the `updateMods`
     * value for an update operation from a `bulkWrite` command.
     */
    const write_ops::UpdateModification& getUpdateMods() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getU();
        } else {
            tassert(7328103, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getUpdateMods();
        }
    }

    /**
     * Returns the `collation` value for the update operation this `UpdateRef` refers to.
     */
    const boost::optional<BSONObj>& getCollation() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->getCollation();
        } else {
            tassert(7328104, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->getCollation();
        }
    }

    /**
     * Returns the BSON representation of the update operation this `UpdateRef` refers to.
     */
    BSONObj toBSON() const {
        if (_batchUpdateRequest) {
            return _batchUpdateRequest->toBSON();
        } else {
            tassert(7328105, "invalid bulkWrite update op reference", _bulkWriteUpdateRequest);
            return _bulkWriteUpdateRequest->toBSON();
        }
    }

private:
    /**
     * Only one of the two of these will be present.
     */
    boost::optional<const mongo::write_ops::UpdateOpEntry&> _batchUpdateRequest;
    boost::optional<const mongo::BulkWriteUpdateOp&> _bulkWriteUpdateRequest;
};

/**
 * Provides access to information for an update operation. Used to abstract over whether a
 * BatchItemRef is pointing to a `mongo::write_ops::DeleteOpEntry` (if it's from a `delete`
 * command) or a `mongo::BulkWriteDeleteOp` (if it's from a `bulkWrite` command).
 */
class DeleteRef {
public:
    DeleteRef() = delete;
    DeleteRef(const mongo::write_ops::DeleteOpEntry& batchDeleteReq)
        : _batchDeleteRequest(batchDeleteReq) {}
    DeleteRef(const mongo::BulkWriteDeleteOp& bulkDeleteReq)
        : _bulkWriteDeleteRequest(bulkDeleteReq) {}

    /**
     * Returns the filter for the delete operation this `DeleteRef` refers to, i.e.  the `q`
     * value for a delete operation from a `delete` command, or the `filter` value for a delete
     * operation from a `bulkWrite` command.
     */
    const BSONObj& getFilter() const {
        if (_batchDeleteRequest) {
            return _batchDeleteRequest->getQ();
        } else {
            tassert(7328106, "invalid bulkWrite delete op reference", _bulkWriteDeleteRequest);
            return _bulkWriteDeleteRequest->getFilter();
        }
    }

    /**
     * Returns the `multi` value for the delete operation this `DeleteRef` refers to.
     */
    bool getMulti() const {
        if (_batchDeleteRequest) {
            return _batchDeleteRequest->getMulti();
        } else {
            tassert(7328107, "invalid bulkWrite delete op reference", _bulkWriteDeleteRequest);
            return _bulkWriteDeleteRequest->getMulti();
        }
    }

    /**
     * Returns the `collation` value for the update operation this `DeleteRef` refers to.
     */
    const boost::optional<BSONObj>& getCollation() const {
        if (_batchDeleteRequest) {
            return _batchDeleteRequest->getCollation();
        } else {
            tassert(7328108, "invalid bulkWrite update op reference", _bulkWriteDeleteRequest);
            return _bulkWriteDeleteRequest->getCollation();
        }
    }

    /**
     * Returns the BSON representation of the dekete operation this `DeleteRef` refers to.
     */
    BSONObj toBSON() const {
        if (_batchDeleteRequest) {
            return _batchDeleteRequest->toBSON();
        } else {
            tassert(7328109, "invalid bulkWrite delete op reference", _bulkWriteDeleteRequest);
            return _bulkWriteDeleteRequest->toBSON();
        }
    }

private:
    /**
     * Only one of the two of these will be present.
     */
    boost::optional<const mongo::write_ops::DeleteOpEntry&> _batchDeleteRequest;
    boost::optional<const mongo::BulkWriteDeleteOp&> _bulkWriteDeleteRequest;
};

/**
 * Similar to above, this class wraps the write items of a command request into a generically usable
 * type. Very thin wrapper, does not own the write item itself.
 *
 * This can wrap write items of a batched insert/update/delete command and a bulkWrite command.
 */
class BatchItemRef {
public:
    BatchItemRef(const BatchedCommandRequest* request, int index);
    BatchItemRef(const BulkWriteCommandRequest* request, int index);

    BatchedCommandRequest::BatchType getOpType() const {
        return _batchType;
    }

    int getItemIndex() const {
        return _index;
    }

    const auto& getDocument() const {
        if (_batchedRequest) {
            return _batchedRequest->getInsertRequest().getDocuments()[_index];
        } else {
            tassert(7263703, "invalid bulkWrite request reference", _bulkWriteRequest);
            const auto& op = _bulkWriteRequest->getOps()[_index];
            return BulkWriteCRUDOp(op).getInsert()->getDocument();
        }
    }

    UpdateRef getUpdateRef() const {
        if (_batchedRequest) {
            return UpdateRef(_batchedRequest->getUpdateRequest().getUpdates()[_index]);
        } else {
            tassert(7263704, "invalid bulkWrite request reference", _bulkWriteRequest);
            auto updateOp = BulkWriteCRUDOp(_bulkWriteRequest->getOps()[_index]).getUpdate();
            tassert(7328111, "bulkWrite op unexpectedly not an update", updateOp);
            return UpdateRef(*updateOp);
        }
    }

    DeleteRef getDeleteRef() const {
        if (_batchedRequest) {
            return DeleteRef(_batchedRequest->getDeleteRequest().getDeletes()[_index]);
        } else {
            tassert(7263705, "invalid bulkWrite request reference", _bulkWriteRequest);
            auto deleteOp = BulkWriteCRUDOp(_bulkWriteRequest->getOps()[_index]).getDelete();
            tassert(7328112, "bulkWrite op unexpectedly not a delete", deleteOp);
            return DeleteRef(*deleteOp);
        }
    }

    auto& getLet() const {
        if (_batchedRequest) {
            return _batchedRequest->getLet();
        } else {
            return _bulkWriteRequest->getLet();
        }
    }

    auto& getLegacyRuntimeConstants() const {
        if (_batchedRequest) {
            return _batchedRequest->getLegacyRuntimeConstants();
        } else {
            // bulkWrite command doesn't support legacy 'runtimeConstants'.
            tassert(7263707, "invalid bulkWrite request reference", _bulkWriteRequest);
            return BatchedCommandRequest::kEmptyRuntimeConstants;
        }
    }

    /**
     * Gets an estimate of how much space, in bytes, the referred-to write operation would add to a
     * batched write command, i.e. insert, update, or delete. This method *must* only be called if
     *  the underlying write op is from an insert/update/delete command. Do not call this method if
     * the underlying write op is from a bulkWrite - use getSizeForBulkWriteBytes() instead.
     */
    int getSizeForBatchWriteBytes() const;

    /**
     * Gets an estimate of how much space, in bytes, the referred-to write operation would add to a
     * bulkWrite command. This method *must* only be called if the underlying write op is from a
     * bulkWrite command. Do not call this method if the underlying write op is from an insert,
     * update, or delete command - use getSizeForBatchWriteBytes() instead.
     */
    int getSizeForBulkWriteBytes() const;

private:
    boost::optional<const BatchedCommandRequest&> _batchedRequest;
    boost::optional<const BulkWriteCommandRequest&> _bulkWriteRequest;
    const int _index;
    /**
     * If this BatchItemRef points to an op in a BatchedCommandRequest, stores the type of the
     * entire batch. If this BatchItemRef points to an op in a BulkWriteRequest, stores the type
     * of this individual op (the batch it belongs to may have a mix of op types.)
     */
    BatchedCommandRequest::BatchType _batchType;
};

BatchedCommandRequest::BatchType convertOpType(BulkWriteCRUDOp::OpType opType);

}  // namespace mongo
