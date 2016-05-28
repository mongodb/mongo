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

#include "mongo/base/disallow_copying.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/write_ops/batched_delete_request.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_request.h"

namespace mongo {

class NamespaceString;

/**
 * This class wraps the different kinds of command requests into a generically usable write
 * command request.
 *
 * Designed to be a very thin wrapper that mimics the underlying requests exactly.  Owns the
 * wrapped request object once constructed.
 */
class BatchedCommandRequest {
    MONGO_DISALLOW_COPYING(BatchedCommandRequest);

public:
    // Maximum number of write ops supported per batch
    static const size_t kMaxWriteBatchSize;

    enum BatchType { BatchType_Insert, BatchType_Update, BatchType_Delete, BatchType_Unknown };

    //
    // construction / destruction
    //

    BatchedCommandRequest(BatchType batchType);

    /**
     * insertReq ownership is transferred to here.
     */
    BatchedCommandRequest(BatchedInsertRequest* insertReq)
        : _batchType(BatchType_Insert), _insertReq(insertReq) {}

    /**
     * updateReq ownership is transferred to here.
     */
    BatchedCommandRequest(BatchedUpdateRequest* updateReq)
        : _batchType(BatchType_Update), _updateReq(updateReq) {}

    /**
     * deleteReq ownership is transferred to here.
     */
    BatchedCommandRequest(BatchedDeleteRequest* deleteReq)
        : _batchType(BatchType_Delete), _deleteReq(deleteReq) {}

    ~BatchedCommandRequest(){};

    /** Copies all the fields present in 'this' to 'other'. */
    void cloneTo(BatchedCommandRequest* other) const;

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(StringData dbName, const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;

    //
    // Batch type accessors
    //

    BatchType getBatchType() const;
    BatchedInsertRequest* getInsertRequest() const;
    BatchedUpdateRequest* getUpdateRequest() const;
    BatchedDeleteRequest* getDeleteRequest() const;
    // Index creation is also an insert, but a weird one.
    bool isInsertIndexRequest() const;
    bool isUniqueIndexRequest() const;
    bool isValidIndexRequest(std::string* errMsg) const;
    std::string getTargetingNS() const;
    const NamespaceString& getTargetingNSS() const;
    BSONObj getIndexKeyPattern() const;

    //
    // individual field accessors
    //

    bool isVerboseWC() const;

    /**
     * Sets the namespace for this batched request.
     */
    void setNS(NamespaceString ns);
    const NamespaceString& getNS() const;

    std::size_t sizeWriteOps() const;

    void setWriteConcern(const BSONObj& writeConcern);
    void unsetWriteConcern();
    bool isWriteConcernSet() const;
    const BSONObj& getWriteConcern() const;

    void setOrdered(bool ordered);
    void unsetOrdered();
    bool isOrderedSet() const;
    bool getOrdered() const;

    void setShardVersion(ChunkVersion shardVersion) {
        _shardVersion = std::move(shardVersion);
    }

    bool hasShardVersion() const {
        return _shardVersion.is_initialized();
    }

    const ChunkVersion& getShardVersion() const {
        return _shardVersion.get();
    }

    void setShouldBypassValidation(bool newVal);
    bool shouldBypassValidation() const;

    //
    // Helpers for batch pre-processing
    //

    /**
     * Generates a new request, the same as the old, but with insert _ids if required.
     * Returns NULL if this is not an insert request or all inserts already have _ids.
     */
    static BatchedCommandRequest* cloneWithIds(const BatchedCommandRequest& origCmdRequest);

    /**
     * Whether or not this batch contains an upsert without an _id - these can't be sent
     * to multiple hosts.
     */
    static bool containsNoIDUpsert(const BatchedCommandRequest& request);

    //
    // Helpers for auth pre-parsing
    //

    /**
     * Helper to determine whether or not there are any upserts in the batch
     */
    static bool containsUpserts(const BSONObj& writeCmdObj);

    /**
     * Helper to extract the namespace being indexed from a raw BSON write command.
     *
     * Returns false with errMsg if the index write command seems invalid.
     * TODO: Remove when we have parsing hooked before authorization
     */
    static bool getIndexedNS(const BSONObj& writeCmdObj,
                             std::string* nsToIndex,
                             std::string* errMsg);

private:
    BatchType _batchType;

    boost::optional<ChunkVersion> _shardVersion;

    std::unique_ptr<BatchedInsertRequest> _insertReq;
    std::unique_ptr<BatchedUpdateRequest> _updateReq;
    std::unique_ptr<BatchedDeleteRequest> _deleteReq;
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

    const BSONObj& getDocument() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getInsertRequest()->getDocumentsAt(_itemIndex);
    }

    const BatchedUpdateDocument* getUpdate() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getUpdateRequest()->getUpdatesAt(_itemIndex);
    }

    const BatchedDeleteDocument* getDelete() const {
        dassert(_itemIndex < static_cast<int>(_request->sizeWriteOps()));
        return _request->getDeleteRequest()->getDeletesAt(_itemIndex);
    }

    BSONObj toBSON() const {
        switch (getOpType()) {
            case BatchedCommandRequest::BatchType_Insert:
                return getDocument();
            case BatchedCommandRequest::BatchType_Update:
                return getUpdate()->toBSON();
            default:
                return getDelete()->toBSON();
        }
    }

private:
    const BatchedCommandRequest* _request;
    const int _itemIndex;
};

}  // namespace mongo
