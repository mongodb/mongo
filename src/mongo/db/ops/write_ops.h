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

#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/uuid.h"

namespace mongo {

class InsertOp {
public:
    static write_ops::InsertCommandRequest parse(const OpMsgRequest& request);
    /**
     * This is to parse OP_INSERT legacy request and deprecated and used only to parse legacy insert
     * request to know how many documents need to be inserted for the purpose of monitoring. Do not
     * call this method any more.
     */
    static write_ops::InsertCommandRequest parseLegacy(const Message& msg);
    static write_ops::InsertCommandReply parseResponse(const BSONObj& obj);
    static void validate(const write_ops::InsertCommandRequest& insertOp);
};

class UpdateOp {
public:
    static write_ops::UpdateCommandRequest parse(const OpMsgRequest& request);
    static write_ops::UpdateCommandReply parseResponse(const BSONObj& obj);
    static void validate(const write_ops::UpdateCommandRequest& updateOp);
};

class DeleteOp {
public:
    static write_ops::DeleteCommandRequest parse(const OpMsgRequest& request);
    static write_ops::DeleteCommandReply parseResponse(const BSONObj& obj);
    static void validate(const write_ops::DeleteCommandRequest& deleteOp);
};

class FindAndModifyOp {
public:
    static write_ops::FindAndModifyCommandReply parseResponse(const BSONObj& obj);
};

namespace write_ops {

// Limit of the number of operations that can be included in a single write command. This is an
// attempt to avoid a large number of errors resulting in a reply that exceeds 16MB. It doesn't
// fully ensure that goal, but it reduces the probability of it happening. This limit should not be
// used if the protocol changes to avoid the 16MB limit on reply size.
constexpr size_t kMaxWriteBatchSize = 100'000;

// Limit the size that we write without yielding to 16MB / 64 (max expected number of indexes)
constexpr size_t insertVectorMaxBytes = 256 * 1024;

// This constant accounts for the size of an individual stmtId, as used for retryable writes.
constexpr size_t kStmtIdSize = 4;

/**
 * Retrieves the statement id for the write at the specified position in the write batch entries
 * array.
 */
int32_t getStmtIdForWriteAt(const WriteCommandRequestBase& writeCommandBase, size_t writePos);

template <class T>
int32_t getStmtIdForWriteAt(const T& op, size_t writePos) {
    return getStmtIdForWriteAt(op.getWriteCommandRequestBase(), writePos);
}

// TODO: Delete this getter once IDL supports defaults for object and array fields
template <class T>
const BSONObj& collationOf(const T& opEntry) {
    static const BSONObj emptyBSON{};
    return opEntry.getCollation().get_value_or(emptyBSON);
}

// TODO: Delete this getter once IDL supports defaults for object and array fields
template <class T>
const std::vector<BSONObj>& arrayFiltersOf(const T& opEntry) {
    static const std::vector<BSONObj> emptyBSONArray{};
    return opEntry.getArrayFilters().get_value_or(emptyBSONArray);
}

/**
 * Set of utilities which estimate the size, in bytes, of an update/delete statement with the given
 * parameters, when serialized in the format used for the update/delete commands.
 */
int getUpdateSizeEstimate(const BSONObj& q,
                          const write_ops::UpdateModification& u,
                          const boost::optional<mongo::BSONObj>& c,
                          bool includeUpsertSupplied,
                          const boost::optional<mongo::BSONObj>& collation,
                          const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters,
                          const boost::optional<mongo::BSONObj>& sort,
                          const mongo::BSONObj& hint,
                          const boost::optional<UUID>& sampleId,
                          bool includeAllowShardKeyUpdatesWithoutFullShardKeyInQuery);
int getDeleteSizeEstimate(const BSONObj& q,
                          const boost::optional<mongo::BSONObj>& collation,
                          const mongo::BSONObj& hint,
                          const boost::optional<UUID>& sampleId);

/**
 * Set of utilities which estimate the size, in bytes, of an insert/update/delete op with the given
 * parameters, when serialized in the format used for the bulkWrite command.
 */
int getBulkWriteInsertSizeEstimate(const mongo::BSONObj& document);
int getBulkWriteUpdateSizeEstimate(const BSONObj& filter,
                                   const write_ops::UpdateModification& updateMods,
                                   const boost::optional<mongo::BSONObj>& constants,
                                   bool includeUpsertSupplied,
                                   const boost::optional<mongo::BSONObj>& collation,
                                   const boost::optional<std::vector<mongo::BSONObj>>& arrayFilters,
                                   const boost::optional<mongo::BSONObj>& sort,
                                   const BSONObj& hint,
                                   const boost::optional<UUID>& sampleId);
int getBulkWriteDeleteSizeEstimate(const BSONObj& filter,
                                   const boost::optional<mongo::BSONObj>& collation,
                                   const mongo::BSONObj& hint,
                                   const boost::optional<UUID>& sampleId);

/**
 * Set of utilities which return true if the estimated write size is greater than or equal to
 * the actual write size, false otherwise.
 *
 * If the caller specifies 'unparsedRequest', these utilities will also return true if the request
 * used document sequences and the size estimate is greater than the maximum size of a BSONObj. This
 * indicates that 'unparsedRequest' cannot be serialized to a BSONObj because it exceeds the maximum
 * BSONObj size.
 */
bool verifySizeEstimate(const write_ops::UpdateOpEntry& update);
bool verifySizeEstimate(const write_ops::DeleteOpEntry& deleteOp);
bool verifySizeEstimate(const InsertCommandRequest& insertReq,
                        const OpMsgRequest* unparsedRequest = nullptr);
bool verifySizeEstimate(const UpdateCommandRequest& updateReq,
                        const OpMsgRequest* unparsedRequest = nullptr);
bool verifySizeEstimate(const DeleteCommandRequest& deleteReq,
                        const OpMsgRequest* unparsedRequest = nullptr);

/**
 * Set of utilities which estimate the size of the headers (that is, all fields in a write command
 * outside of the write statements themselves) of an insert/update/delete command, respectively.
 */
int getInsertHeaderSizeEstimate(const InsertCommandRequest& insertReq);
int getUpdateHeaderSizeEstimate(const UpdateCommandRequest& updateReq);
int getDeleteHeaderSizeEstimate(const DeleteCommandRequest& deleteReq);

/**
 * If the response from a write command contains any write errors, it will throw the first one. All
 * the remaining errors will be disregarded.
 *
 * Usages of this utility for anything other than single-document writes would be suspicious due to
 * the fact that it will swallow the remaining ones.
 */
void checkWriteErrors(const WriteCommandReplyBase& reply);

template <class T>
T checkWriteErrors(T op) {
    checkWriteErrors(op.getWriteCommandReplyBase());
    return std::move(op);
}

}  // namespace write_ops
}  // namespace mongo
