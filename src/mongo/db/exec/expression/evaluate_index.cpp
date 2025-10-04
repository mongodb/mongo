/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/key_string/key_string.h"

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionToHashedIndexKey& expr, const Document& root, Variables* variables) {
    Value inpVal(expr.getChildren()[0]->evaluate(root, variables));
    if (inpVal.missing()) {
        inpVal = Value(BSONNULL);
    }

    return Value(BSONElementHasher::hash64(BSON("" << inpVal).firstElement(),
                                           BSONElementHasher::DEFAULT_HASH_SEED));
}

Value evaluate(const ExpressionInternalKeyStringValue& expr,
               const Document& root,
               Variables* variables) {
    const Value input = expr.getInput()->evaluate(root, variables);
    auto inputBson = input.wrap("");

    std::unique_ptr<CollatorInterface> collator = nullptr;
    if (auto collationExpr = expr.getCollation(); collationExpr) {
        const Value collation = collationExpr->evaluate(root, variables);
        uassert(8281503,
                str::stream() << "Collation spec must be an object, not "
                              << typeName(collation.getType()),
                collation.isObject());
        auto collationBson = collation.getDocument().toBson();

        auto collatorFactory = CollatorFactoryInterface::get(
            expr.getExpressionContext()->getOperationContext()->getServiceContext());
        collator = uassertStatusOKWithContext(collatorFactory->makeFromBSON(collationBson),
                                              "Invalid collation spec");
    }

    key_string::HeapBuilder ksBuilder(key_string::Version::V1);
    if (collator) {
        ksBuilder.appendBSONElement(inputBson.firstElement(), [&](StringData str) {
            return collator->getComparisonString(str);
        });
    } else {
        ksBuilder.appendBSONElement(inputBson.firstElement());
    }

    // The result omits the typebits so that the numeric value of different types have the same
    // binary representation.
    auto ksValue = ksBuilder.release();
    auto ksView = ksValue.getView();
    return Value(BSONBinData{ksView.data(), static_cast<int>(ksView.size()), BinDataGeneral});
}

}  // namespace exec::expression
}  // namespace mongo
