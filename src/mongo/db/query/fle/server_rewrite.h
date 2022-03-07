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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/util/assert_util.h"

namespace mongo::fle {

class FLEFindRewriter {
public:
    using GetDocumentByIdCallback = std::function<BSONObj(BSONElement _id)>;

    // The public constructor takes a reference to EncryptionInformation since it should never be
    // null, but it stores a pointer since mock rewriters for tests might not need
    // EncryptionInformation.
    FLEFindRewriter(const EncryptionInformation& info, GetDocumentByIdCallback callback)
        : _info(&info), _getById(callback) {
        invariant(callback);
    }


    /**
     * Rewrites a match expression with FLE find payloads into a disjunction on the __safeContent__
     * array of tags. Can rewrite the passed-in MatchExpression in place, which is why
     * a unique_ptr is required.
     *
     * Will rewrite top-level $eq and $in expressions, as well as recursing through $and, $or,
     * $not and $nor. All other MatchExpressions, notably $elemMatch, are ignored.
     */
    std::unique_ptr<MatchExpression> rewriteMatchExpression(std::unique_ptr<MatchExpression> expr);

    /**
     * Determine whether a given BSONElement is in fact a FLE find payload.
     * Sub-type 6, sub-sub-type 0x05.
     */
    virtual bool isFleFindPayload(const BSONElement& elt) {
        if (!elt.isBinData(BinDataType::Encrypt)) {
            return false;
        }
        int dataLen;
        auto data = elt.binData(dataLen);
        return dataLen >= 1 &&
            data[0] == static_cast<uint8_t>(EncryptedBinDataType::kFLE2FindEqualityPayload);
    }

protected:
    // The default constructor should only be used for mocks in testing.
    FLEFindRewriter() : _info(nullptr), _getById(nullptr) {}

private:
    /**
     * A single rewrite step, used in the expression walker as well as at the top-level.
     */
    std::unique_ptr<MatchExpression> _rewrite(MatchExpression* me);
    // BSONElements are always owned by a BSONObj. So we just return the owning BSONObj and can
    // extract the elements out later.
    // TODO: SERVER-63293
    virtual BSONObj rewritePayloadAsTags(BSONElement fleFindPayload);
    std::unique_ptr<InMatchExpression> rewriteEq(const EqualityMatchExpression* expr);
    std::unique_ptr<InMatchExpression> rewriteIn(const InMatchExpression* expr);

    // Holds a pointer so that it can be null for tests, even though the public constructor
    // takes a const reference.
    const EncryptionInformation* _info;
    GetDocumentByIdCallback _getById;
};

}  // namespace mongo::fle
