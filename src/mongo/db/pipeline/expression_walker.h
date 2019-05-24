/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/pipeline/expression.h"

namespace mongo::expression_walker {

/**
 * Provided with a Walker and an Expression, walk() calls each of the following:
 * * walker.preVisit() once before walking to each child.
 * * walker.inVisit() between walking to each child. It is called multiple times, once between each
 *   pair of children. walker.inVisit() is skipped if the Expression has fewer than two children.
 * * walker.postVisit() once after walking to each child.
 * Each of the Expression's child Expressions is recursively walked and the same three methods are
 * called for it.
 */
template <typename Walker>
void walk(Walker* walker, Expression* expression) {
    if (expression) {
        walker->preVisit(expression);

        // InVisit needs to be called between every two nodes which requires more complicated
        // branching logic.
        auto count = 0ull;
        for (auto&& child : expression->getChildren()) {
            if (count != 0ull)
                walker->inVisit(count, expression);
            ++count;
            walk(walker, child.get());
        }

        walker->postVisit(expression);
    }
}

}  // namespace mongo::expression_walker
