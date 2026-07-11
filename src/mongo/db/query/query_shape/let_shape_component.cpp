// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/let_shape_component.h"

namespace mongo::query_shape {

namespace {
BSONObj extractLetShape(BSONObj letSpec,
                        const query_shape::SerializationOptions& opts,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (letSpec.isEmpty()) {
        // Fast path for the common case.
        return BSONObj();
    }

    BSONObjBuilder bob;
    for (BSONElement elem : letSpec) {
        // Exclude built in variables in let expressions. Let expressions with system variables are
        // only added by a router and have known reparse errors. The variables will appear in the
        // query shape on the node where the command originated.
        if (!Variables::isBuiltin(elem.fieldName())) {
            auto expr = Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState);
            auto redactedValue = expr->serialize(opts);
            // Note that this will throw on deeply nested let variables.
            redactedValue.addToBsonObj(&bob, opts.serializeFieldPathFromString(elem.fieldName()));
        }
    }
    return bob.obj();
}

auto representativeLetShape(boost::optional<BSONObj> let,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return let ? extractLetShape(
                     *let,
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     expCtx)
               : BSONObj();
}
}  // namespace

LetShapeComponent::LetShapeComponent(boost::optional<BSONObj> let,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : shapifiedLet(representativeLetShape(let, expCtx)), hasLet(bool(let)) {}

void LetShapeComponent::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), hasLet, simpleHash(shapifiedLet));
}

size_t LetShapeComponent::size() const {
    return sizeof(LetShapeComponent) + shapifiedLet.objsize();
}

void LetShapeComponent::appendTo(BSONObjBuilder& bob,
                                 const query_shape::SerializationOptions& opts,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    if (!hasLet) {
        return;
    }

    auto shapeToAppend = shapifiedLet;
    if (opts != query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have the representative query cached/stored here, but the caller is asking for a
        // different format, so we must re-compute.
        shapeToAppend = extractLetShape(shapifiedLet, opts, expCtx);
    }
    bob.append(FindCommandRequest::kLetFieldName, shapeToAppend);
}
}  // namespace mongo::query_shape
