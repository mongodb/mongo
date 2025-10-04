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

#pragma once

#include "mongo/base/string_data.h"
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

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * The $project stage can be used for simple transformations such as including or excluding a set
 * of fields, or can do more sophisticated things, like include some fields and add new "computed"
 * fields, using the expression language. Note you can not mix an exclusion-style projection with
 * adding or including any other fields.
 */
class DocumentSourceProject final {
public:
    static constexpr StringData kStageName = "$project"_sd;
    static constexpr StringData kAliasNameUnset = "$unset"_sd;

    /**
     * Method to create a $project stage from a Projection AST.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        projection_ast::Projection projection,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        StringData specifiedName);

    /**
     * Convenience method to create a $project stage from 'projectSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj projectSpec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        StringData specifiedName) try {

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
