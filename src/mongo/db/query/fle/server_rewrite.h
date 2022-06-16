/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/transaction_api.h"

namespace mongo {
class FLEQueryInterface;
namespace fle {

/**
 * Low Selectivity rewrites use $expr which is not supported in all commands such as upserts.
 */
enum class HighCardinalityModeAllowed {
    kAllow,
    kDisallow,
};

/**
 * Make a collator object from its BSON representation. Useful when creating ExpressionContext
 * objects for parsing MatchExpressions as part of the server-side rewrite.
 */
std::unique_ptr<CollatorInterface> collatorFromBSON(OperationContext* opCtx,
                                                    const BSONObj& collation);

/**
 * Return a rewritten version of the passed-in filter as a BSONObj. Generally used by other
 * functions to process MatchExpressions in each command.
 */
BSONObj rewriteQuery(OperationContext* opCtx,
                     boost::intrusive_ptr<ExpressionContext> expCtx,
                     const NamespaceString& nss,
                     const EncryptionInformation& info,
                     BSONObj filter,
                     GetTxnCallback getTransaction,
                     HighCardinalityModeAllowed mode);

/**
 * Process a find command with encryptionInformation in-place, rewriting the filter condition so
 * that any query on an encrypted field will properly query the underlying tags array.
 */
void processFindCommand(OperationContext* opCtx,
                        const NamespaceString& nss,
                        FindCommandRequest* findCommand,
                        GetTxnCallback txn);

/**
 * Process a count command with encryptionInformation in-place, rewriting the filter condition so
 * that any query on an encrypted field will properly query the underlying tags array.
 */
void processCountCommand(OperationContext* opCtx,
                         const NamespaceString& nss,
                         CountCommandRequest* countCommand,
                         GetTxnCallback getTxn);

/**
 * Process a pipeline with encryptionInformation by rewriting the pipeline to query against the
 * underlying tags array, where appropriate. After this rewriting is complete, there is no more
 * FLE work to be done. The encryption info does not need to be kept around (e.g. on a command
 * object).
 */
std::unique_ptr<Pipeline, PipelineDeleter> processPipeline(
    OperationContext* opCtx,
    NamespaceString nss,
    const EncryptionInformation& encryptInfo,
    std::unique_ptr<Pipeline, PipelineDeleter> toRewrite,
    GetTxnCallback txn);

/**
 * Rewrite a filter MatchExpression with FLE Find Payloads into a disjunction over the tag array
 * from inside an existing transaction using a FLEQueryInterface constructed from a
 * transaction client.
 */
BSONObj rewriteEncryptedFilterInsideTxn(
    FLEQueryInterface* queryImpl,
    StringData db,
    const EncryptedFieldConfig& efc,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    BSONObj filter,
    HighCardinalityModeAllowed mode = HighCardinalityModeAllowed::kDisallow);

/**
 * Class which handles rewriting filter MatchExpressions for FLE2. The functionality is encapsulated
 * as a class rather than just a namespace so that the collection readers don't have to be passed
 * around as extra arguments to every function.
 *
 * Exposed in the header file for unit testing purposes. External callers should use the
 * rewriteEncryptedFilterInsideTxn() helper function defined above.
 */
class FLEQueryRewriter {
public:
    enum class HighCardinalityMode {
        // Always use high cardinality filters, used by tests
        kForceAlways,

        // Use high cardinality mode if $in rewrites do not fit in the
        // internalQueryFLERewriteMemoryLimit memory limit
        kUseIfNeeded,

        // Do not rewrite into high cardinality filter, throw exceptions instead
        // Some contexts like upsert do not support $expr
        kDisallow,
    };

    /**
     * Takes in references to collection readers for the ESC and ECC that are used during tag
     * computation.
     */
    FLEQueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx,
                     const FLEStateCollectionReader& escReader,
                     const FLEStateCollectionReader& eccReader,
                     HighCardinalityModeAllowed mode = HighCardinalityModeAllowed::kAllow)
        : _expCtx(expCtx), _escReader(&escReader), _eccReader(&eccReader) {

        if (internalQueryFLEAlwaysUseHighCardinalityMode.load()) {
            _mode = HighCardinalityMode::kForceAlways;
        }

        if (mode == HighCardinalityModeAllowed::kDisallow) {
            _mode = HighCardinalityMode::kDisallow;
        }

        // This isn't the "real" query so we don't want to increment Expression
        // counters here.
        _expCtx->stopExpressionCounters();
    }

    /**
     * Accepts a BSONObj holding a MatchExpression, and returns BSON representing the rewritten
     * expression. Returns boost::none if no rewriting was done.
     *
     * Rewrites the match expression with FLE find payloads into a disjunction on the
     * __safeContent__ array of tags.
     *
     * Will rewrite top-level $eq and $in expressions, as well as recursing through $and, $or, $not
     * and $nor. Also handles similarly limited rewriting under $expr. All other MatchExpressions,
     * notably $elemMatch, are ignored.
     */
    boost::optional<BSONObj> rewriteMatchExpression(const BSONObj& filter);

    /**
     * Accepts an expression to be re-written. Will rewrite top-level expressions including $eq and
     * $in, as well as recursing through other expressions. Returns a new pointer if the top-level
     * expression must be changed. A nullptr indicates that the modifications happened in-place.
     */
    std::unique_ptr<Expression> rewriteExpression(Expression* expression);

    /**
     * Determine whether a given BSONElement is in fact a FLE find payload.
     * Sub-type 6, sub-sub-type 0x05.
     */
    virtual bool isFleFindPayload(const BSONElement& elt) const {
        if (!elt.isBinData(BinDataType::Encrypt)) {
            return false;
        }
        int dataLen;
        auto data = elt.binData(dataLen);
        return dataLen >= 1 &&
            data[0] == static_cast<uint8_t>(EncryptedBinDataType::kFLE2FindEqualityPayload);
    }

    /**
     * Determine whether a given Value is in fact a FLE find payload.
     * Sub-type 6, sub-sub-type 0x05.
     */
    bool isFleFindPayload(const Value& v) const {
        if (v.getType() != BSONType::BinData) {
            return false;
        }

        auto binData = v.getBinData();
        return binData.type == BinDataType::Encrypt && binData.length >= 1 &&
            static_cast<uint8_t>(EncryptedBinDataType::kFLE2FindEqualityPayload) ==
            static_cast<const uint8_t*>(binData.data)[0];
    }

    std::vector<Value> rewritePayloadAsTags(Value fleFindPayload) const;

    ExpressionContext* expCtx() {
        return _expCtx.get();
    }

    bool isForceHighCardinality() const {
        return _mode == HighCardinalityMode::kForceAlways;
    }

    void setForceHighCardinalityForTest() {
        _mode = HighCardinalityMode::kForceAlways;
    }

    HighCardinalityMode getHighCardinalityMode() const {
        return _mode;
    }

protected:
    // This constructor should only be used for mocks in testing.
    FLEQueryRewriter(boost::intrusive_ptr<ExpressionContext> expCtx)
        : _expCtx(expCtx), _escReader(nullptr), _eccReader(nullptr) {}

private:
    /**
     * A single rewrite step, called recursively on child expressions.
     */
    std::unique_ptr<MatchExpression> _rewrite(MatchExpression* me);

    virtual BSONObj rewritePayloadAsTags(BSONElement fleFindPayload) const;
    std::unique_ptr<MatchExpression> rewriteEq(const EqualityMatchExpression* expr);
    std::unique_ptr<MatchExpression> rewriteIn(const InMatchExpression* expr);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    // Holds a pointer so that these can be null for tests, even though the public constructor
    // takes a const reference.
    const FLEStateCollectionReader* _escReader;
    const FLEStateCollectionReader* _eccReader;

    // True if the last Expression or MatchExpression processed by this rewriter was rewritten.
    bool _rewroteLastExpression = false;

    // Controls how query rewriter rewrites the query
    HighCardinalityMode _mode{HighCardinalityMode::kUseIfNeeded};
};


}  // namespace fle
}  // namespace mongo
