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

#include <algorithm>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <iterator>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/cst/cst_match_translation.h"
#include "mongo/db/cst/cst_pipeline_translation.h"
#include "mongo/db/cst/key_fieldname.h"
#include "mongo/db/cst/key_value.h"

namespace mongo::cst_match_translation {
namespace {

std::unique_ptr<MatchExpression> translateMatchElement(const CNode::Fieldname& field,
                                                       const CNode& cst) {
    if (auto fieldName = stdx::get_if<UserFieldname>(&field)) {
        // Expression is over a user fieldname.
        return stdx::visit(
            visit_helper::Overloaded{
                [&](const CNode::ObjectChildren& userObject) -> std::unique_ptr<MatchExpression> {
                    MONGO_UNREACHABLE;
                },
                [&](const CNode::ArrayChildren& userObject) -> std::unique_ptr<MatchExpression> {
                    MONGO_UNREACHABLE;
                },
                // Other types are always treated as equality predicates.
                [&](auto&& userValue) -> std::unique_ptr<MatchExpression> {
                    return std::make_unique<EqualityMatchExpression>(
                        StringData(*fieldName),
                        cst_pipeline_translation::translateLiteralLeaf(cst));
                }},
            cst.payload);
    } else {
        // Top level match expression.
    }
    MONGO_UNREACHABLE;
}

}  // namespace

std::unique_ptr<MatchExpression> translateMatchExpression(
    const CNode& cst, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto root = std::make_unique<AndMatchExpression>();
    for (auto&& [field, expr] : cst.objectChildren()) {
        root->add(translateMatchElement(field, expr).release());
    }
    return root;
}

}  // namespace mongo::cst_match_translation
