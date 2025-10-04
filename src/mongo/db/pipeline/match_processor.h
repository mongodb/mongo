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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression_algo.h"

#include <memory>

#include <boost/intrusive_ptr.hpp>

namespace mongo {

// This class is used by the aggregation framework and streams enterprise module
// to perform the document processing needed for $match.
class MatchProcessor {
public:
    MatchProcessor(std::unique_ptr<MatchExpression> expr,
                   DepsTracker dependencies,
                   BSONObj&& predicate);

    // Processes the given document and returns true if it matches the conditions specified in
    // the MatchExpression.
    bool process(const Document& input) const;

    std::unique_ptr<MatchExpression>& getExpression() {
        return _expression;
    }

    const std::unique_ptr<MatchExpression>& getExpression() const {
        return _expression;
    }

    void setExpression(std::unique_ptr<MatchExpression> expression) {
        _expression = std::move(expression);
    }

    const BSONObj& getPredicate() const {
        return _predicate;
    }

private:
    // Determines whether all fields have unique prefixes. This is called once during object
    // construction to determine the value of '_hasUniquePrefixes'.
    static bool hasUniquePrefixes(const OrderedPathSet& fields);

    std::unique_ptr<MatchExpression> _expression;

    // Cache the dependencies so that we know what fields we need to serialize to BSON for
    // matching.
    DepsTracker _dependencies;

    // Whether or not the paths in '_dependencies.fields' have unique prefixes or not. Based on the
    // uniqueness check outcome the match processor may be able to use an optimized code path when
    // converting input Documents to BSONObjs.
    const bool _hasUniquePrefixes;

    // Store the BSONObj that backs this '_expression' so that it doesn't get disposed before the
    // match expression does.
    BSONObj _predicate;
};

}  // namespace mongo
