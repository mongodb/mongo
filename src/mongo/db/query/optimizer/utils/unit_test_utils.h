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

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

void maybePrintABT(const ABT& abt);

#define ASSERT_EXPLAIN(expected, abt) \
    maybePrintABT(abt);               \
    ASSERT_EQ(expected, ExplainGenerator::explain(abt))

#define ASSERT_EXPLAIN_V2(expected, abt) \
    maybePrintABT(abt);                  \
    ASSERT_EQ(expected, ExplainGenerator::explainV2(abt))

#define ASSERT_EXPLAIN_V2Compact(expected, abt) \
    maybePrintABT(abt);                         \
    ASSERT_EQ(expected, ExplainGenerator::explainV2Compact(abt))

#define ASSERT_EXPLAIN_BSON(expected, abt) \
    maybePrintABT(abt);                    \
    ASSERT_EQ(expected, ExplainGenerator::explainBSON(abt))

#define ASSERT_EXPLAIN_PROPS_V2(expected, phaseManager)                              \
    ASSERT_EQ(expected,                                                              \
              ExplainGenerator::explainV2(                                           \
                  make<MemoPhysicalDelegatorNode>(phaseManager.getPhysicalNodeId()), \
                  true /*displayPhysicalProperties*/,                                \
                  &phaseManager.getMemo()))

#define ASSERT_EXPLAIN_MEMO(expected, memo) ASSERT_EQ(expected, ExplainGenerator::explainMemo(memo))

#define ASSERT_BSON_PATH(expected, bson, path)                      \
    ASSERT_EQ(expected,                                             \
              dotted_path_support::extractElementAtPath(bson, path) \
                  .toString(false /*includeFieldName*/));


#define ASSERT_BETWEEN(a, b, value) \
    ASSERT_LTE(a, value);           \
    ASSERT_GTE(b, value);

struct TestIndexField {
    FieldNameType fieldName;
    CollationOp op;
    bool isMultiKey;
};

ABT makeIndexPath(FieldPathType fieldPath, bool isMultiKey = true);

ABT makeIndexPath(FieldNameType fieldName);
ABT makeNonMultikeyIndexPath(FieldNameType fieldName);

IndexDefinition makeIndexDefinition(FieldNameType fieldName,
                                    CollationOp op,
                                    bool isMultiKey = true);
IndexDefinition makeCompositeIndexDefinition(std::vector<TestIndexField> indexFields,
                                             bool isMultiKey = true);

ABT translatePipeline(const Metadata& metadata,
                      const std::string& pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss = {});

ABT translatePipeline(Metadata& metadata,
                      const std::string& pipelineStr,
                      std::string scanDefName,
                      PrefixId& prefixId);

ABT translatePipeline(const std::string& pipelineStr, std::string scanDefName = "collection");

}  // namespace mongo::optimizer
