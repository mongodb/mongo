/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/let_shape_component.h"

namespace mongo::query_shape {

namespace {
BSONObj extractLetShape(BSONObj letSpec,
                        const SerializationOptions& opts,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (letSpec.isEmpty()) {
        // Fast path for the common case.
        return BSONObj();
    }

    BSONObjBuilder bob;
    for (BSONElement elem : letSpec) {
        auto expr = Expression::parseOperand(expCtx.get(), elem, expCtx->variablesParseState);
        auto redactedValue = expr->serialize(opts);
        // Note that this will throw on deeply nested let variables.
        redactedValue.addToBsonObj(&bob, opts.serializeFieldPathFromString(elem.fieldName()));
    }
    return bob.obj();
}

auto representativeLetShape(boost::optional<BSONObj> let,
                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return let ? extractLetShape(
                     *let, SerializationOptions::kRepresentativeQueryShapeSerializeOptions, expCtx)
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
                                 const SerializationOptions& opts,
                                 const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    if (!hasLet) {
        return;
    }

    auto shapeToAppend = shapifiedLet;
    if (opts != SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
        // We have the representative query cached/stored here, but the caller is asking for a
        // different format, so we must re-compute.
        shapeToAppend = extractLetShape(shapifiedLet, opts, expCtx);
    }
    bob.append(FindCommandRequest::kLetFieldName, shapeToAppend);
}
}  // namespace mongo::query_shape
