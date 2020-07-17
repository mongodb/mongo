/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <iterator>

#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo::cst_pipeline_translation {
namespace {
/**
 * Walk a projection CNode and produce a ProjectionASTNode.
 */
auto translateProjection(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    // TODO SERVER-48834: Support more than inclusion projection.
    return std::make_unique<BooleanConstantASTNode>(true);
}

/**
 * Walk a project stage object CNode and produce a DocumentSourceSingleDocumentTransformation.
 */
auto translateProject(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    using namespace projection_ast;
    auto root = ProjectionPathASTNode{};
    bool sawId = false;

    for (auto&& [name, child] : cst.objectChildren()) {
        if (stdx::get<UserFieldname>(name) == UserFieldname{"_id"})
            sawId = true;
        addNodeAtPath(&root, stdx::get<UserFieldname>(name), translateProjection(child, expCtx));
    }

    if (!sawId)
        addNodeAtPath(&root, "_id", std::make_unique<BooleanConstantASTNode>(true));

    return DocumentSourceProject::create(
        // TODO SERVER-48834: Support more than inclusion projection.
        Projection{root, ProjectType::kInclusion},
        expCtx,
        "$project");
}

/**
 * Cast a CNode payload to a UserLong.
 */
auto translateNumToLong(const CNode& cst) {
    return stdx::visit(
        visit_helper::Overloaded{
            [](const UserDouble& userDouble) {
                return (BSON("" << userDouble).firstElement()).safeNumberLong();
            },
            [](const UserInt& userInt) {
                return (BSON("" << userInt).firstElement()).safeNumberLong();
            },
            [](const UserLong& userLong) { return userLong; },
            [](auto &&) -> UserLong { MONGO_UNREACHABLE }},
        cst.payload);
}

/**
 * Walk a skip stage object CNode and produce a DocumentSourceSkip.
 */
auto translateSkip(const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    UserLong nToSkip = translateNumToLong(cst);
    return DocumentSourceSkip::create(expCtx, nToSkip);
}

/**
 * Walk an aggregation pipeline stage object CNode and produce a DocumentSource.
 */
boost::intrusive_ptr<DocumentSource> translateSource(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    switch (cst.firstKeyFieldname()) {
        case KeyFieldname::project:
            return translateProject(cst.objectChildren()[0].second, expCtx);
        case KeyFieldname::skip:
            return translateSkip(cst.objectChildren()[0].second, expCtx);
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

/**
 * Walk a pipeline array CNode and produce a Pipeline.
 */
std::unique_ptr<Pipeline, PipelineDeleter> translatePipeline(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto sources = Pipeline::SourceContainer{};
    static_cast<void>(std::transform(cst.arrayChildren().begin(),
                                     cst.arrayChildren().end(),
                                     std::back_inserter(sources),
                                     [&](auto&& elem) { return translateSource(elem, expCtx); }));
    return Pipeline::create(std::move(sources), expCtx);
}

}  // namespace mongo::cst_pipeline_translation
