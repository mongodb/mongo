// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_project.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/transformer_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <bitset>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(project,
                                     ProjectLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);
REGISTER_LITE_PARSED_DOCUMENT_SOURCE(unset,
                                     ProjectLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(project,
                                                   DocumentSourceProject,
                                                   ProjectStageParams);

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
    std::string_view specifiedName) {
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
                ex.addContext("Invalid " + std::string{specifiedName});
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
        uassert(15969, "$project specification must be an object", elem.type() == BSONType::object);
        return DocumentSourceProject::create(elem.Obj(), expCtx, elem.fieldNameStringData());
    }

    tassert(11282972,
            str::stream() << "Attempting to parse DocumentSourceProject from neither " << kStageName
                          << ", nor " << kAliasNameUnset << " BSON object",
            elem.fieldNameStringData() == kAliasNameUnset);
    uassert(31002,
            "$unset specification must be a string or an array",
            (elem.type() == BSONType::array || elem.type() == BSONType::string));

    const auto unsetSpec =
        elem.type() == BSONType::array ? elem.Array() : std::vector<mongo::BSONElement>{1, elem};
    uassert(31119,
            "$unset specification must be a string or an array with at least one field",
            unsetSpec.size() > 0);

    uassert(31120,
            "$unset specification must be a string or an array containing only string values",
            std::all_of(unsetSpec.cbegin(), unsetSpec.cend(), [](BSONElement elem) {
                return elem.type() == BSONType::string;
            }));
    return DocumentSourceProject::create(
        buildExclusionProjectionSpecification(unsetSpec), expCtx, elem.fieldNameStringData());
}

}  // namespace mongo
