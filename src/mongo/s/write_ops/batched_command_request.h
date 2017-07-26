/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/ops/write_ops.h"
#include "mongo/s/chunk_version.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {

/**
 * This class wraps the different kinds of command requests into a generically usable write command
 * request that can be passed around.
 */
class BatchedCommandRequest {
public:
    enum BatchType { BatchType_Insert, BatchType_Update, BatchType_Delete };

    BatchedCommandRequest(write_ops::Insert insertOp)
        : _batchType(BatchType_Insert),
          _insertReq(stdx::make_unique<write_ops::Insert>(std::move(insertOp))) {}

    BatchedCommandRequest(write_ops::Update updateOp)
        : _batchType(BatchType_Update),
          _updateReq(stdx::make_unique<write_ops::Update>(std::move(updateOp))) {}

    BatchedCommandRequest(write_ops::Delete deleteOp)
        : _batchType(BatchType_Delete),
          _deleteReq(stdx::make_unique<write_ops::Delete>(std::move(deleteOp))) {}

    BatchedCommandRequest(BatchedCommandRequest&&) = default;

    static BatchedCommandRequest parseInsert(const OpMsgRequest& request);
    static BatchedCommandRequest parseUpdate(const OpMsgRequest& request);
    static BatchedCommandRequest parseDelete(const OpMsgRequest& request);

    BatchType getBatchType() const {
        return _batchType;
    }

    const NamespaceString& getNS() const;
    NamespaceString getTargetingNS() const;

    /**
     * Index creation can be expressed as an insert into the 'system.indexes' namespace.
     */
    bool isInsertIndexRequest() const;

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

    std::size_t sizeWriteOps() const;

    void setWriteConcern(const BSONObj& writeConcern) {
        _writeConcern = writeConcern.getOwned();
    }

    bool hasWriteConcern() const {
        return _writeConcern.is_initialized();
    }

    const BSONObj& getWriteConcern() const {
        invariant(_writeConcern);
        return *_writeConcern;
    }

    bool isVerboseWC() const;

    void setShardVersion(ChunkVersion shardVersion) {
        _shardVersion = std::move(shardVersion);
    }

    bool hasShardVersion() const {
        return _shardVersion.is_initialized();
    }

    const ChunkVersion& getShardVersion() const {
        invariant(_shardVersion);
        return *_shardVersion;
    }

    const write_ops::WriteCommandBase& getWriteCommandBase() const;
    void setWriteCommandBase(write_ops::WriteCommandBase writeCommandBase);

    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;
    std::string toString() const;

    /**
     * Generates a new request, the same as the old, but with insert _ids if required.
     */
    static BatchedCommandRequest cloneInsertWithIds(BatchedCommandRequest origCmdRequest);

private:
    BatchType _batchType;

    std::unique_ptr<write_ops::Insert> _insertReq;
    std::unique_ptr<write_ops::Update> _updateReq;
    std::unique_ptr<write_ops::Delete> _deleteReq;

    boost::optional<ChunkVersion> _shardVersion;

    boost::optional<BSONObj> _writeConcern;
};

/**
 * Similar to above, this class wraps the write items of a command request into a generically
 * usable type.  Very thin wrapper, does not own the write item itself.
 *
 * TODO: Use in BatchedCommandRequest above
 */
class BatchItemRef {
public:
    BatchItemRef(const BatchedCommandRequest* request, int itemIndex)
        : _request(request), _itemIndex(itemIndex) {}

    const BatchedCommandRequest* getRequest() const {
        return _request;
    }

    int getItemIndex() const {
        return _itemIndex;
    }

    BatchedCommandRequest::BatchType getOpType() const {
        return _request->getBatchType();
    }

    const auto& getDocument() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getInsertRequest().getDocuments().at(_itemIndex);
    }

    const auto& getUpdate() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getUpdateRequest().getUpdates().at(_itemIndex);
    }

    const auto& getDelete() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getDeleteRequest().getDeletes().at(_itemIndex);
    }

private:
    const BatchedCommandRequest* _request;
    const int _itemIndex;
};

}  // namespace mongo
