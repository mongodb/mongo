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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_text_base.h"
#include "mongo/db/matcher/expression_where_base.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace MONGO_MOD_PUB mongo {

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

}  // namespace MONGO_MOD_PUB mongo
