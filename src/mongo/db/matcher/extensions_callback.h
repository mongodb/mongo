// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Certain match clauses (the "extension" clauses, namely $text and $where) require context in
 * order to perform parsing. This context is captured inside of an ExtensionsCallback object.
 */
class ExtensionsCallback {
public:
    virtual ~ExtensionsCallback() {}

    virtual std::unique_ptr<MatchExpression> createText(
        TextMatchExpressionBase::TextParams text) const = 0;
    virtual std::unique_ptr<MatchExpression> createWhere(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WhereMatchExpressionBase::WhereParams where) const = 0;

    /**
     * Returns true if extensions (e.g. $text and $where) are allowed but are converted into no-ops.
     *
     * Queries with a no-op extension context are special because they can be parsed and planned,
     * but they cannot be executed.
     */
    virtual bool hasNoopExtensions() const {
        return false;
    }

    // Convenience wrappers for BSON.
    StatusWithMatchExpression parseText(BSONElement text) const;
    StatusWithMatchExpression parseWhere(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         BSONElement where) const;

protected:
    /**
     * Helper method which extracts parameters from the given $text element.
     */
    static StatusWith<TextMatchExpressionBase::TextParams> extractTextMatchExpressionParams(
        BSONElement text);

    /**
     * Helper method which extracts parameters from the given $where element.
     */
    static StatusWith<WhereMatchExpressionBase::WhereParams> extractWhereMatchExpressionParams(
        BSONElement where);
};

}  // namespace mongo
