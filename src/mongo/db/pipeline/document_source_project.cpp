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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_project.h"

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>

#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(project,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceProject::createFromBson,
                         LiteParsedDocumentSource::AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE(unset,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceProject::createFromBson,
                         LiteParsedDocumentSource::AllowedWithApiStrict::kAlways);

namespace {
BSONObj buildExclusionProjectionSpecification(const std::vector<BSONElement>& unsetSpec) {
    BSONObjBuilder objBuilder;
    for (const auto& elem : unsetSpec) {
        objBuilder << elem.valueStringData() << 0;
    }
    return objBuilder.obj();
}
}  // namespace

intrusive_ptr<DocumentSource> DocumentSourceProject::create(
    projection_ast::Projection projection,
    const intrusive_ptr<ExpressionContext>& expCtx,
    StringData specifiedName) {
    const bool isIndependentOfAnyCollection = false;
    intrusive_ptr<DocumentSource> project(new DocumentSourceSingleDocumentTransformation(
        expCtx,
        [&]() {
            // The ProjectionExecutor will internally perform a check to see if the provided
            // specification is valid, and throw an exception if it was not. The exception is caught
            // here so we can add the name that was actually specified by the user, be it $project
            // or an alias.
            try {
                // We won't optimize the executor on creation, and will do it as part of the
                // pipeline optimization process when requested via the 'optimize()' method on
                // 'DocumentSourceSingleDocumentTransformation', so we won't pass the
                // 'kOptimzeExecutor' flag to the projection executor builder.
                //
                // Note that this is also important for $lookup inner pipelines to not being
                // optimized too early, as it may lead to incorrect positioning of the caching
                // stage due to missing dependencies on certain variables, as they could have been
                // optimized away.
                auto builderParams = projection_executor::BuilderParamsBitSet{
                    projection_executor::kDefaultBuilderParams};
                builderParams.reset(projection_executor::kOptimizeExecutor);
                return projection_executor::buildProjectionExecutor(
                    expCtx,
                    &projection,
                    ProjectionPolicies::aggregateProjectionPolicies(),
                    builderParams);
            } catch (DBException& ex) {
                ex.addContext("Invalid " + specifiedName.toString());
                throw;
            }
        }(),
        kStageName,
        isIndependentOfAnyCollection));
    return project;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceProject::createUnset(
    const FieldPath& fieldPath, const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    // This helper is only meant for removing top-level fields. Dotted field paths require
    // thinking about implicit array traversal.
    tassert(5339701,
            str::stream() << "Expected a top-level field name, but got " << fieldPath.fullPath(),
            fieldPath.getPathLength() == 1);

    projection_ast::ProjectionPathASTNode pathNode;
    pathNode.addChild(fieldPath.fullPath(),
                      std::make_unique<projection_ast::BooleanConstantASTNode>(false));
    auto projection = projection_ast::Projection{
        std::move(pathNode),
        projection_ast::ProjectType::kExclusion,
    };

    return create(std::move(projection), expCtx, kAliasNameUnset);
}

intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    if (elem.fieldNameStringData() == kStageName) {
        uassert(15969, "$project specification must be an object", elem.type() == BSONType::Object);
        return DocumentSourceProject::create(elem.Obj(), expCtx, elem.fieldNameStringData());
    }

    invariant(elem.fieldNameStringData() == kAliasNameUnset);
    uassert(31002,
            "$unset specification must be a string or an array",
            (elem.type() == BSONType::Array || elem.type() == BSONType::String));

    const auto unsetSpec =
        elem.type() == BSONType::Array ? elem.Array() : std::vector<mongo::BSONElement>{1, elem};
    uassert(31119,
            "$unset specification must be a string or an array with at least one field",
            unsetSpec.size() > 0);

    uassert(31120,
            "$unset specification must be a string or an array containing only string values",
            std::all_of(unsetSpec.cbegin(), unsetSpec.cend(), [](BSONElement elem) {
                return elem.type() == BSONType::String;
            }));
    return DocumentSourceProject::create(
        buildExclusionProjectionSpecification(unsetSpec), expCtx, elem.fieldNameStringData());
}

}  // namespace mongo
