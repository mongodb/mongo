// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * ExtensionsCallbackNoop does not capture any context, and produces "no op" expressions that can't
 * be used for matching.  It should be used when parsing context is not available (for example, when
 * the relevant namespace does not exist, or in mongos).
 */
class ExtensionsCallbackNoop : public ExtensionsCallback {
public:
    std::unique_ptr<MatchExpression> createText(
        TextMatchExpressionBase::TextParams text) const final;

    std::unique_ptr<MatchExpression> createWhere(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        WhereMatchExpressionBase::WhereParams where) const final;

    bool hasNoopExtensions() const final {
        return true;
    }
};

}  // namespace mongo
