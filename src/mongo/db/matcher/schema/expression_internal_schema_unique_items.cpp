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

#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"

#include "mongo/bson/bsonobjbuilder.h"

#include <boost/move/utility_core.hpp>

namespace mongo {
constexpr StringData InternalSchemaUniqueItemsMatchExpression::kName;

void InternalSchemaUniqueItemsMatchExpression::debugString(StringBuilder& debug,
                                                           int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);

    BSONObjBuilder builder;
    serialize(&builder, {});
    debug << builder.obj().toString();
    _debugStringAttachTagInfo(&debug);
}

bool InternalSchemaUniqueItemsMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalSchemaUniqueItemsMatchExpression*>(expr);
    return path() == other->path();
}

void InternalSchemaUniqueItemsMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const SerializationOptions& opts, bool includePath) const {
    bob->append(kName, true);
}

std::unique_ptr<MatchExpression> InternalSchemaUniqueItemsMatchExpression::clone() const {
    auto clone =
        std::make_unique<InternalSchemaUniqueItemsMatchExpression>(path(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return clone;
}
}  // namespace mongo
