// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/matcher.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

Matcher::Matcher(const BSONObj& pattern,
                 const boost::intrusive_ptr<ExpressionContext>& expCtx,
                 const ExtensionsCallback& extensionsCallback,
                 const MatchExpressionParser::AllowedFeatureSet allowedFeatures)
    : _pattern(pattern) {
    _expression = uassertStatusOK(
        MatchExpressionParser::parse(pattern, expCtx, extensionsCallback, allowedFeatures));
}

}  // namespace mongo
