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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"

namespace mongo {

/**
 * Builds the cursor field and the _latestOplogTimestamp field for a reply to a cursor-generating
 * command in place.
 */
class CursorResponseBuilder {
    CursorResponseBuilder(const CursorResponseBuilder&) = delete;
    CursorResponseBuilder& operator=(const CursorResponseBuilder&) = delete;

public:
    /**
     * Structure used to confiugre the CursorResponseBuilder.
     */
    struct Options {
        bool isInitialResponse = false;
        bool useDocumentSequences = false;
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
        return _options.useDocumentSequences ? _docSeqBuilder->len() : _batch->len();
    }

    void append(const BSONObj& obj) {
        invariant(_active);
        if (_options.useDocumentSequences) {
            _docSeqBuilder->append(obj);
        } else {
            _batch->append(obj);
        }
        _numDocs++;
    }

    void setLatestOplogTimestamp(Timestamp ts) {
        _latestOplogTimestamp = ts;
    }

    void setPostBatchResumeToken(BSONObj token) {
        _postBatchResumeToken = token.getOwned();
    }

    long long numDocs() const {
        return _numDocs;
    }

    /**
     * Call this after successfully appending all fields that will be part of this response.
     * After calling, you may not call any more methods on this object.
     */
    void done(CursorId cursorId, StringData cursorNamespace);

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
    boost::optional<OpMsgBuilder::DocSequenceBuilder> _docSeqBuilder;

    bool _active = true;
    long long _numDocs = 0;
    Timestamp _latestOplogTimestamp;
    BSONObj _postBatchResumeToken;
};

/**
 * Builds a cursor response object from the provided cursor identifiers and "firstBatch",
 * and appends the response object to the provided builder under the field name "cursor".
 *
 * The response object has the following format:
 *   { id: <NumberLong>, ns: <String>, firstBatch: <Array> }.
 *
 * This function is deprecated.  Prefer CursorResponseBuilder or CursorResponse::toBSON() instead.
 */
void appendCursorResponseObject(long long cursorId,
                                StringData cursorNamespace,
                                BSONArray firstBatch,
                                BSONObjBuilder* builder);

/**
 * Builds a getMore response object from the provided cursor identifiers and "nextBatch",
 * and appends the response object to the provided builder under the field name "cursor".
 *
 * The response object has the following format:
 *   { id: <NumberLong>, ns: <String>, nextBatch: <Array> }.
 *
 * This function is deprecated.  Prefer CursorResponseBuilder or CursorResponse::toBSON() instead.
 */
void appendGetMoreResponseObject(long long cursorId,
                                 StringData cursorNamespace,
                                 BSONArray nextBatch,
                                 BSONObjBuilder* builder);

class CursorResponse {
// In order to work around a bug in the compiler on the s390x platform, the IDL needs to invoke the
// copy constructor on that platform.
// TODO SERVER-32467 Remove this ifndef once the compiler has been fixed and the workaround has been
// removed.
#ifndef __s390x__
    CursorResponse(const CursorResponse&) = delete;
    CursorResponse& operator=(const CursorResponse&) = delete;
#endif

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
     * Constructs a CursorResponse from the command BSON response.
     */
    static StatusWith<CursorResponse> parseFromBSON(const BSONObj& cmdResponse);

    /**
     * A throwing version of 'parseFromBSON'.
     */
    static CursorResponse parseFromBSONThrowing(const BSONObj& cmdResponse) {
        return uassertStatusOK(parseFromBSON(cmdResponse));
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
                   boost::optional<long long> numReturnedSoFar = boost::none,
                   boost::optional<Timestamp> latestOplogTimestamp = boost::none,
                   boost::optional<BSONObj> postBatchResumeToken = boost::none,
                   boost::optional<BSONObj> writeConcernError = boost::none);

    CursorResponse(CursorResponse&& other) = default;
    CursorResponse& operator=(CursorResponse&& other) = default;

// In order to work around a bug in the compiler on the s390x platform, the IDL needs to invoke the
// copy constructor on that platform.
// TODO SERVER-32467 Remove this ifndef once the compiler has been fixed and the workaround has been
// removed.
#ifdef __s390x__
    CursorResponse(const CursorResponse& other) = default;
    CursorResponse& operator=(const CursorResponse& other) = default;
#endif

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

    boost::optional<long long> getNumReturnedSoFar() const {
        return _numReturnedSoFar;
    }

    boost::optional<Timestamp> getLastOplogTimestamp() const {
        return _latestOplogTimestamp;
    }

    boost::optional<BSONObj> getPostBatchResumeToken() const {
        return _postBatchResumeToken;
    }

    boost::optional<BSONObj> getWriteConcernError() const {
        return _writeConcernError;
    }

    /**
     * Converts this response to its raw BSON representation.
     */
    BSONObj toBSON(ResponseType responseType) const;
    void addToBSON(ResponseType responseType, BSONObjBuilder* builder) const;
    BSONObj toBSONAsInitialResponse() const {
        return toBSON(ResponseType::InitialResponse);
    }

private:
    NamespaceString _nss;
    CursorId _cursorId;
    std::vector<BSONObj> _batch;
    boost::optional<long long> _numReturnedSoFar;
    boost::optional<Timestamp> _latestOplogTimestamp;
    boost::optional<BSONObj> _postBatchResumeToken;
    boost::optional<BSONObj> _writeConcernError;
};

}  // namespace mongo
