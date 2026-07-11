// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class NamespaceString;
class OperationContext;

class TextMatchExpression : public TextMatchExpressionBase {
public:
    explicit TextMatchExpression(fts::FTSQueryImpl ftsQuery);
    TextMatchExpression(OperationContext* opCtx, const NamespaceString& nss, TextParams params);

    const fts::FTSQuery& getFTSQuery() const final {
        return _ftsQuery;
    }

    std::unique_ptr<MatchExpression> clone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    fts::FTSQueryImpl _ftsQuery;
};

}  // namespace mongo
