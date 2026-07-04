/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/query_settings.h"

#include "mongo/db/query/query_settings/query_settings_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo::query_settings {

namespace {
const QuerySettings kEmpty{};
}  // namespace

const QuerySettings& forOp(OperationContext* opCtx) {
    using namespace query_settings_details;
    return std::visit(
        OverloadedVisitor{
            // No eligible command has begun: no settings apply.
            [&](const NotStarted&) -> const QuerySettings& { return kEmpty; },
            // Eligible but not yet resolved: reading now would observe stale values.
            [&](const Pending&) -> const QuerySettings& {
                tasserted(13020703, "query settings read while resolution is still pending");
            },
            // Resolution matched nothing: no settings apply.
            [&](const Empty&) -> const QuerySettings& { return kEmpty; },
            [&](const QuerySettings& settings) -> const QuerySettings& { return settings; },
        },
        getQuerySettingsStateForOp(opCtx));
}

}  // namespace mongo::query_settings
