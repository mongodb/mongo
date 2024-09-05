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

#include <cstdint>

#include "mongo/db/pipeline/abt/algebrizer_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_registry.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/path_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

struct ABTDocumentSourceTranslationVisitorContext : public DocumentSourceVisitorContextBase {
    ABTDocumentSourceTranslationVisitorContext(AlgebrizerContext& ctx, size_t maxFilterDepth)
        : algCtx(ctx), maxFilterDepth(maxFilterDepth) {}

    // Prevent copying and moving as this object has reference members.
    ABTDocumentSourceTranslationVisitorContext(const ABTDocumentSourceTranslationVisitorContext&) =
        delete;
    ABTDocumentSourceTranslationVisitorContext& operator=(
        const ABTDocumentSourceTranslationVisitorContext&) = delete;
    ABTDocumentSourceTranslationVisitorContext(ABTDocumentSourceTranslationVisitorContext&&) =
        delete;
    ABTDocumentSourceTranslationVisitorContext& operator=(
        ABTDocumentSourceTranslationVisitorContext&&) = delete;

    void pushLimitSkip(int64_t limit, int64_t skip);

    AlgebrizerContext& algCtx;

    // This configures the maximum number of FilterNodes that a single FilterNode may be split into.
    const size_t maxFilterDepth;
};

ABT translatePipelineToABT(const Pipeline& pipeline,
                           ProjectionName scanProjName,
                           ABT initialNode,
                           PrefixId& prefixId,
                           QueryParameterMap& queryParameters,
                           size_t maxFilterDepth = kMaxPathConjunctionDecomposition);

}  // namespace mongo::optimizer
