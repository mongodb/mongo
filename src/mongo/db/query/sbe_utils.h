/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/sbe_stage_builder.h"

namespace mongo::sbe {
/**
 * Checks if the given query can be executed with the SBE engine.
 */
bool isQuerySbeCompatible(const CollectionPtr* collection,
                          const CanonicalQuery* cq,
                          size_t plannerOptions);

/**
 * Some of InputParamIds assigned in the MatchExpression tree (parameter 'root') might be missing
 * from 'inputParamToSlotMap' due to later optimizations on index bound evaluation step. This
 * function validates that in this case the missing parameters are indeed missing due to the
 * optimizations mentioned above and, as such, are not represented in Interval Evaluation Tree built
 * during index bound evaluation and stored in 'inputParamToSlotMap'.
 */
bool validateInputParamsBindings(
    const MatchExpression* root,
    const std::vector<stage_builder::IndexBoundsEvaluationInfo>& indexBoundsEvaluationInfos,
    const stage_builder::InputParamToSlotMap& inputParamToSlotMap);
}  // namespace mongo::sbe
