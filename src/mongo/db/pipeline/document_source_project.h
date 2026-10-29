// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Project);

/**
 * The $project stage can be used for simple transformations such as including or excluding a set
 * of fields, or can do more sophisticated things, like include some fields and add new "computed"
 * fields, using the expression language. Note you can not mix an exclusion-style projection with
 * adding or including any other fields.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceProject final {
public:
    static constexpr std::string_view kStageName = "$project"sv;
    static constexpr std::string_view kAliasNameUnset = "$unset"sv;

    /**
     * Method to create a $project stage from a Projection AST.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        projection_ast::Projection projection,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::string_view specifiedName);

    /**
     * Convenience method to create a $project stage from 'projectSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj projectSpec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::string_view specifiedName) try {

        auto projection = projection_ast::parseAndAnalyze(
            expCtx, projectSpec, ProjectionPolicies::aggregateProjectionPolicies());
        return create(projection, expCtx, specifiedName);
    } catch (DBException& ex) {
        ex.addContext("Invalid " + std::string{specifiedName});
        throw;
    }

    /**
     * Create an '$unset' stage, which removes a single top-level field.
     *
     * 'fieldPath' must be a top-level field.
     */
    static boost::intrusive_ptr<DocumentSource> createUnset(
        const FieldPath& fieldPath, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $project stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    // It is illegal to construct a DocumentSourceProject directly, use create() or createFromBson()
    // instead.
    DocumentSourceProject() = default;
};

}  // namespace mongo
