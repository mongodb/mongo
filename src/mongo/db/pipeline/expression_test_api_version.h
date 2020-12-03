/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/expression.h"

namespace mongo {
/**
 * This expression will be used to validate that versioning code is working as expected.
 * $_testApiVersion should only take one parameter, either {unstable: true} or {deprecated: true}.
 * If no error is thrown, this expression will return an integer value.
 */
class ExpressionTestApiVersion final : public Expression {
public:
    static constexpr auto kUnstableField = "unstable";
    static constexpr auto kDeprecatedField = "deprecated";

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* const expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState& vps);

    Value evaluate(const Document& root, Variables* variables) const final;

    Value serialize(bool explain) const final;

    void acceptVisitor(ExpressionVisitor* visitor) final {
        return visitor->visit(this);
    }

private:
    ExpressionTestApiVersion(ExpressionContext* const expCtx, bool unstable, bool deprecated);
    void _doAddDependencies(DepsTracker* deps) const final override;

    bool _unstable;
    bool _deprecated;
};
}  // namespace mongo
