/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression.h"

namespace mongo {
namespace change_stream_rewrite {
/**
 * Rewrites a 'userMatch' that will filter events which have been transformed into change stream
 * format so that it can be applied directly to oplog entries. The resulting MatchExpression will
 * preemptively filter out some oplog entries that we know in advance will be filtered out by the
 * 'userMatch' filter, reducing the amount of work that subsequent change streams stage will have to
 * do.
 *
 * The rewrites will only be performed on fields which are present in the 'includeFields' set and
 * absent from the 'excludeFields' set. When 'includeFields' is the empty set, the rewrite defaults
 * to including all paths that can be rewritten.
 */
std::unique_ptr<MatchExpression> rewriteFilterForFields(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* userMatch,
    std::set<std::string> includeFields = {},
    std::set<std::string> excludeFields = {});
}  // namespace change_stream_rewrite
}  // namespace mongo
