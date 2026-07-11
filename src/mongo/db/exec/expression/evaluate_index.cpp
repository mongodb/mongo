// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/hasher.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/storage/key_string/key_string.h"

#include <string_view>

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionToHashedIndexKey& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    Value inpVal(expr.getChildren()[0]->evaluate(root, variables, ctx));
    if (inpVal.missing()) {
        inpVal = Value(BSONNULL);
    }

    return Value(BSONElementHasher::hash64(BSON("" << inpVal).firstElement(),
                                           BSONElementHasher::DEFAULT_HASH_SEED));
}

Value evaluate(const ExpressionInternalKeyStringValue& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    const Value input = expr.getInput()->evaluate(root, variables, ctx);
    auto inputBson = input.wrap("");

    std::unique_ptr<CollatorInterface> collator = nullptr;
    if (auto collationExpr = expr.getCollation(); collationExpr) {
        const Value collation = collationExpr->evaluate(root, variables, ctx);
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
        ksBuilder.appendBSONElement(inputBson.firstElement(), [&](std::string_view str) {
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
