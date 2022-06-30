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

#include "mongo/db/query/optimizer/utils/unit_test_utils.h"

#include "mongo/db/pipeline/abt/document_source_visitor.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {

static constexpr bool kDebugAsserts = false;

void maybePrintABT(const ABT& abt) {
    // Always print using the supported versions to make sure we don't crash.
    const std::string strV1 = ExplainGenerator::explain(abt);
    const std::string strV2 = ExplainGenerator::explainV2(abt);
    auto [tag, val] = ExplainGenerator::explainBSON(abt);
    sbe::value::ValueGuard vg(tag, val);

    if constexpr (kDebugAsserts) {
        std::cout << "V1: " << strV1 << "\n";
        std::cout << "V2: " << strV2 << "\n";
        std::cout << "BSON: " << ExplainGenerator::printBSON(tag, val) << "\n";
    }
}

ABT makeIndexPath(FieldPathType fieldPath, bool isMultiKey) {
    ABT result = make<PathIdentity>();

    for (size_t i = fieldPath.size(); i-- > 0;) {
        if (isMultiKey) {
            result = make<PathTraverse>(std::move(result));
        }
        result = make<PathGet>(std::move(fieldPath.at(i)), std::move(result));
    }

    return result;
}

ABT makeIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)});
}

ABT makeNonMultikeyIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)}, false /*isMultiKey*/);
}

IndexDefinition makeIndexDefinition(FieldNameType fieldName, CollationOp op, bool isMultiKey) {
    IndexCollationSpec idxCollSpec{
        IndexCollationEntry((isMultiKey ? makeIndexPath(std::move(fieldName))
                                        : makeNonMultikeyIndexPath(std::move(fieldName))),
                            op)};
    return IndexDefinition{std::move(idxCollSpec), isMultiKey};
}

IndexDefinition makeCompositeIndexDefinition(std::vector<TestIndexField> indexFields,
                                             bool isMultiKey) {
    IndexCollationSpec idxCollSpec;
    for (auto& idxField : indexFields) {
        idxCollSpec.emplace_back((idxField.isMultiKey
                                      ? makeIndexPath(std::move(idxField.fieldName))
                                      : makeNonMultikeyIndexPath(std::move(idxField.fieldName))),
                                 idxField.op);
    }
    return IndexDefinition{std::move(idxCollSpec), isMultiKey};
}

std::unique_ptr<mongo::Pipeline, mongo::PipelineDeleter> parsePipeline(
    const NamespaceString& nss,
    const std::string& inputPipeline,
    OperationContextNoop& opCtx,
    const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss) {
    const BSONObj inputBson = fromjson("{pipeline: " + inputPipeline + "}");

    std::vector<BSONObj> rawPipeline;
    for (auto&& stageElem : inputBson["pipeline"].Array()) {
        ASSERT_EQUALS(stageElem.type(), BSONType::Object);
        rawPipeline.push_back(stageElem.embeddedObject());
    }

    AggregateCommandRequest request(nss, rawPipeline);
    boost::intrusive_ptr<ExpressionContextForTest> ctx(
        new ExpressionContextForTest(&opCtx, request));

    // Setup the resolved namespaces for other involved collections.
    for (const auto& resolvedNss : involvedNss) {
        ctx->setResolvedNamespace(resolvedNss.ns, resolvedNss);
    }

    unittest::TempDir tempDir("ABTPipelineTest");
    ctx->tempDir = tempDir.path();

    return Pipeline::parse(request.getPipeline(), ctx);
}

ABT translatePipeline(const Metadata& metadata,
                      const std::string& pipelineStr,
                      ProjectionName scanProjName,
                      std::string scanDefName,
                      PrefixId& prefixId,
                      const std::vector<ExpressionContext::ResolvedNamespace>& involvedNss) {
    OperationContextNoop opCtx;
    auto pipeline =
        parsePipeline(NamespaceString("a." + scanDefName), pipelineStr, opCtx, involvedNss);
    return translatePipelineToABT(metadata,
                                  *pipeline.get(),
                                  scanProjName,
                                  make<ScanNode>(scanProjName, std::move(scanDefName)),
                                  prefixId);
}

ABT translatePipeline(Metadata& metadata,
                      const std::string& pipelineStr,
                      std::string scanDefName,
                      PrefixId& prefixId) {
    return translatePipeline(
        metadata, pipelineStr, prefixId.getNextId("scan"), scanDefName, prefixId);
}

ABT translatePipeline(const std::string& pipelineStr, std::string scanDefName) {
    PrefixId prefixId;
    Metadata metadata{{}};
    return translatePipeline(metadata, pipelineStr, std::move(scanDefName), prefixId);
}

}  // namespace mongo::optimizer
