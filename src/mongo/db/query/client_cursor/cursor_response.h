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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Builds the cursor field for a reply to a cursor-generating command in-place.
 */
class CursorResponseBuilder {
    CursorResponseBuilder(const CursorResponseBuilder&) = delete;
    CursorResponseBuilder& operator=(const CursorResponseBuilder&) = delete;

public:
    /**
     * Structure used to configure the CursorResponseBuilder.
     *
     * If we selected atClusterTime or received it from the client, transmit it back to the client
     * in the cursor reply document by setting it here.
     */
    struct Options {
        bool isInitialResponse = false;
        boost::optional<LogicalTime> atClusterTime = boost::none;
    };

    /**
     * Once constructed, you may not use the passed-in ReplyBuilderInterface until you call either
     * done()
     * or abandon(), or this object goes out of scope. This is the same as the rule when using a
     * BSONObjBuilder to build a sub-object with subobjStart().
     *
     * If the builder goes out of scope without a call to done(), the ReplyBuilderInterface will be
     * reset.
     */
    CursorResponseBuilder(rpc::ReplyBuilderInterface* replyBuilder, Options options);

    ~CursorResponseBuilder() {
        if (_active)
            abandon();
    }

    size_t bytesUsed() const {
        invariant(_active);
        return _batch->len();
    }

    MONGO_COMPILER_ALWAYS_INLINE void append(const BSONObj& obj) {
        invariant(_active);

        _batch->append(obj);
        _numDocs++;
    }

    void setPostBatchResumeToken(BSONObj token) {
        _postBatchResumeToken = token.isEmptyPrototype() ? token : token.getOwned();
    }

    void setPartialResultsReturned(bool partialResults) {
        _partialResultsReturned = partialResults;
    }

    void setInvalidated() {
        _invalidated = true;
    }

    void setWasStatementExecuted(bool wasStatementExecuted) {
        _wasStatementExecuted = wasStatementExecuted;
    }

    long long numDocs() const {
        return _numDocs;
    }

    void reserveReplyBuffer(size_t bytes) {
        if (_replyBuilder != nullptr) {
            _replyBuilder->reserveBytes(bytes);
        }
    }

    /**
     * Call this after successfully appending all fields that will be part of this response.
     * After calling, you may not call any more methods on this object.
     */
    void done(CursorId cursorId,
              const NamespaceString& cursorNamespace,
              boost::optional<CursorMetrics> metrics = boost::none,
              const SerializationContext& serializationContext =
                  SerializationContext::stateCommandReply());

    /**
     * Call this if the response should not contain cursor information. It will completely remove
     * the cursor field from the commandResponse, as if the CursorResponseBuilder was never used.
     * After calling, you may not call any more methods on this object.
     */
    void abandon();

private:
    const Options _options;
    rpc::ReplyBuilderInterface* const _replyBuilder;
    // Order here is important to ensure destruction in the correct order.
    boost::optional<BSONObjBuilder> _bodyBuilder;
    boost::optional<BSONObjBuilder> _cursorObject;
    boost::optional<BSONArrayBuilder> _batch;

    bool _active = true;
    long long _numDocs = 0;
    BSONObj _postBatchResumeToken;
    bool _partialResultsReturned = false;
    bool _invalidated = false;
    bool _wasStatementExecuted = false;
};

/**
 * Builds a cursor response object from the provided cursor identifiers and "firstBatch",
 * and appends the response object to the provided builder under the field name "cursor".
 *
 * The response object has the following format:
 *   { id: <NumberLong>, ns: <String>, firstBatch: <Array> , type: <String>}.
 *
 * The type field is optional, but can be used to differentiate cursors if multiple are returned
 * at once.
 *
 * This function is deprecated.  Prefer CursorResponseBuilder or CursorResponse::toBSON() instead.
 */
void appendCursorResponseObject(
    long long cursorId,
    const NamespaceString& cursorNamespace,
    BSONArray firstBatch,
    boost::optional<StringData> cursorType,
    BSONObjBuilder* builder,
    const SerializationContext& serializationContext = SerializationContext::stateCommandReply());

class CursorResponse {
public:
    enum class ResponseType {
        InitialResponse,
        SubsequentResponse,
    };

    /**
     * Constructs a vector of CursorResponses from a command BSON response that represents one or
     * more cursors.
     */
    static std::vector<StatusWith<CursorResponse>> parseFromBSONMany(const BSONObj& cmdResponse);

    /**
     * Constructs a CursorResponse from the command BSON response. If 'cmdResponse' is not owned,
     * the second argument should be the object that owns the response.
     */
    static StatusWith<CursorResponse> parseFromBSON(
        const BSONObj& cmdResponse,
        const BSONObj* ownedObj = nullptr,
        boost::optional<TenantId> tenantId = boost::none,
        const SerializationContext& serializationContext = SerializationContext());

    /**
     * A throwing version of 'parseFromBSON'.
     */
    static CursorResponse parseFromBSONThrowing(
        boost::optional<TenantId> tenantId,
        const BSONObj& cmdResponse,
        const SerializationContext& serializationContext = SerializationContext()) {
        return uassertStatusOK(parseFromBSON(cmdResponse, nullptr, tenantId, serializationContext));
    }

    /**
     * Constructs an empty cursor response.
     */
    CursorResponse() = default;

    /**
     * Constructs from values for each of the fields.
     */
    CursorResponse(NamespaceString nss,
                   CursorId cursorId,
                   std::vector<BSONObj> batch,
                   boost::optional<Timestamp> atClusterTime = boost::none,
                   boost::optional<BSONObj> postBatchResumeToken = boost::none,
                   boost::optional<BSONObj> writeConcernError = boost::none,
                   boost::optional<BSONObj> varsField = boost::none,
                   boost::optional<BSONObj> explain = boost::none,
                   boost::optional<CursorTypeEnum> cursorType = boost::none,
                   boost::optional<CursorMetrics> metrics = boost::none,
                   bool partialResultsReturned = false,
                   bool invalidated = false,
                   bool wasStatementExecuted = false);

    CursorResponse(CursorResponse&& other) = default;
    CursorResponse& operator=(CursorResponse&& other) = default;

    //
    // Accessors.
    //

    const NamespaceString& getNSS() const {
        return _nss;
    }

    CursorId getCursorId() const {
        return _cursorId;
    }

    const std::vector<BSONObj>& getBatch() const {
        return _batch;
    }

    std::vector<BSONObj> releaseBatch() {
        return std::move(_batch);
    }

    boost::optional<BSONObj> getPostBatchResumeToken() const {
        return _postBatchResumeToken;
    }

    boost::optional<BSONObj> getWriteConcernError() const {
        return _writeConcernError;
    }

    boost::optional<Timestamp> getAtClusterTime() const {
        return _atClusterTime;
    }

    boost::optional<BSONObj> getVarsField() const {
        return _varsField;
    }

    boost::optional<BSONObj> getExplain() const {
        return _explain;
    }

    auto& getCursorMetrics() const {
        return _metrics;
    }

    auto getCursorType() const {
        return _cursorType;
    }

    bool getPartialResultsReturned() const {
        return _partialResultsReturned;
    }

    bool getInvalidated() const {
        return _invalidated;
    }

    bool getWasStatementExecuted() const {
        return _wasStatementExecuted;
    }

    /**
     * Converts this response to its raw BSON representation.
     */
    BSONObj toBSON(ResponseType responseType,
                   const SerializationContext& serializationContext = SerializationContext()) const;
    void addToBSON(ResponseType responseType,
                   BSONObjBuilder* builder,
                   const SerializationContext& serializationContext = SerializationContext()) const;
    BSONObj toBSONAsInitialResponse(
        const SerializationContext& serializationContext = SerializationContext()) const {
        return toBSON(ResponseType::InitialResponse, serializationContext);
    }

private:
    NamespaceString _nss;
    CursorId _cursorId;
    std::vector<BSONObj> _batch;
    boost::optional<Timestamp> _atClusterTime;
    boost::optional<BSONObj> _postBatchResumeToken;
    boost::optional<BSONObj> _writeConcernError;
    boost::optional<BSONObj> _varsField;
    boost::optional<BSONObj> _explain;
    boost::optional<CursorTypeEnum> _cursorType;
    boost::optional<CursorMetrics> _metrics;
    bool _partialResultsReturned = false;
    bool _invalidated = false;
    bool _wasStatementExecuted = false;
};

}  // namespace mongo
