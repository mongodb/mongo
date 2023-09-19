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

#include "mongo/db/matcher/expression_dnf_converter.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
std::pair<boolean_simplification::Maxterm, std::vector<ExpressionBitInfo>> transformToDNF(
    const MatchExpression* root) {
    LOGV2_DEBUG(7767000,
                5,
                "Converting MatchExpression to corresponding DNF",
                "expression"_attr = root->debugString());

    auto [bitsetRoot, expressions] = transformToBitsetTree(root);

    auto maxterm = boolean_simplification::convertToDNF(bitsetRoot);
    maxterm.removeRedundancies();

    LOGV2_DEBUG(
        7767001, 5, "MatchExpression in DNF representation", "maxterm"_attr = maxterm.toString());

    return {std::move(maxterm), std::move(expressions)};
}
}  // namespace mongo
