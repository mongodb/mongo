// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(AddFields);

/**
 * $addFields adds or replaces the specified fields to/in the document while preserving the original
 * document. It is modeled on and throws the same errors as $project.
 *
 * This stage is also aliased as $set and functions the same way.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DocumentSourceAddFields final {
public:
    static constexpr std::string_view kStageName{"$addFields"};
    static constexpr std::string_view kAliasNameSet{"$set"};  // An alternate name for this stage.

    /**
     * Convenience method for creating a $addFields stage from 'addFieldsSpec'.
     */
    static boost::intrusive_ptr<DocumentSource> create(
        BSONObj addFieldsSpec,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        std::string_view stageName = kStageName);

    /**
     * Create a stage that binds an expression to a top-level field.
     *
     * 'fieldPath' must be a top-level field name (exactly one element; no dots).
     */
    static boost::intrusive_ptr<DocumentSource> create(
        const FieldPath& fieldPath,
        const boost::intrusive_ptr<Expression>& expr,
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

    /**
     * Parses a $addFields stage from the user-supplied BSON.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

private:
    // It is illegal to construct a DocumentSourceAddFields directly, use create() or
    // createFromBson() instead.
    DocumentSourceAddFields() = default;
};

}  // namespace mongo
