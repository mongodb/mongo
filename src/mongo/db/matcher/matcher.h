// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

class CollatorInterface;

/**
 * Matcher is a simple wrapper around a BSONObj and the MatchExpression created from it.
 *
 * TODO SERVER-113198: Remove external dependencies on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] Matcher {
    Matcher(const Matcher&) = delete;
    Matcher& operator=(const Matcher&) = delete;

public:
    /**
     * 'collator' must outlive the returned Matcher and any MatchExpression cloned from it.
     */
    Matcher(const BSONObj& pattern,
            const boost::intrusive_ptr<ExpressionContext>& expCtx,
            const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
            MatchExpressionParser::AllowedFeatureSet allowedFeatures =
                MatchExpressionParser::kDefaultSpecialFeatures);

    const BSONObj* getQuery() const {
        return &_pattern;
    }

    std::string toString() const {
        return _pattern.toString();
    }

    MatchExpression* getMatchExpression() {
        return _expression.get();
    }

    const MatchExpression* getMatchExpression() const {
        return _expression.get();
    }

private:
    BSONObj _pattern;

    std::unique_ptr<MatchExpression> _expression;
};

}  // namespace mongo
