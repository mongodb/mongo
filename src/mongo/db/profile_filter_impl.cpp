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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/profile_filter_impl.h"

namespace mongo {

namespace {
boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
    // The namespace string can't be database-specific, because the profile filter applies to all
    // databases in the process.
    // Similar to collection validators, it's not safe to share an opCtx in a stored query.
    return make_intrusive<ExpressionContext>(nullptr, nullptr, NamespaceString{});
}
}  // namespace

ProfileFilterImpl::ProfileFilterImpl(BSONObj expr) : _matcher(expr.getOwned(), makeExpCtx()) {
    DepsTracker deps;
    _matcher.getMatchExpression()->addDependencies(&deps);
    uassert(4910201,
            "Profile filter is not allowed to depend on metadata",
            !deps.getNeedsAnyMetadata());

    // Reduce the DepsTracker down to a set of top-level fields.
    StringSet toplevelFields;
    for (auto&& field : deps.fields) {
        toplevelFields.emplace(FieldPath(std::move(field)).front());
    }

    // Remember a list of functions we'll call whenever we need to build BSON from CurOp.
    _makeBSON = OpDebug::appendStaged(toplevelFields, deps.needWholeDocument);
}

bool ProfileFilterImpl::matches(OperationContext* opCtx,
                                const OpDebug& op,
                                const CurOp& curop) const {
    try {
        return _matcher.matches(_makeBSON({opCtx, op, curop}));
    } catch (const DBException& e) {
        LOGV2_DEBUG(4910202, 5, "Profile filter threw an exception", "exception"_attr = e);
        return false;
    }
}

// PathlessOperatorMap is required for parsing a MatchExpression.
MONGO_INITIALIZER_GENERAL(ProfileFilterDefault,
                          ("PathlessOperatorMap", "MatchExpressionParser"),
                          ())
(InitializerContext*) {
    try {
        if (auto expr = serverGlobalParams.defaultProfileFilter) {
            ProfileFilter::setDefault(std::make_shared<ProfileFilterImpl>(*expr));
        }
        return Status::OK();
    } catch (AssertionException& e) {
        // Add more context to the error
        uasserted(ErrorCodes::FailedToParse,
                  str::stream() << "Failed to parse option operationProfiling.filter: "
                                << e.reason());
    }
}

}  // namespace mongo
